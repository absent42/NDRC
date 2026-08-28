# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""The expected-divergence registry.

NDRC must be byte-identical to reference DRC by default, which means
reproducing DRC's defects as well as its behaviour. Where a defect is
fixed instead, the difference is recorded here with the ruling that
authorised it. An unregistered difference is a failure, always.

Nothing diverges silently. That is the whole point of the file.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

DEFAULT_REGISTRY = Path(__file__).resolve().parent.parent / "divergences.json"


@dataclass(frozen=True)
class Divergence:
    id: str
    fixture: str        # fixture stem, or "*" for any
    target: str         # target name, or "*" for any
    byte_start: int
    byte_end: int       # exclusive
    cause: str
    ruling: str

    def covers(self, fixture: str, target: str, offset: int) -> bool:
        if self.fixture != "*" and self.fixture != fixture:
            return False
        if self.target != "*" and self.target != target:
            return False
        return self.byte_start <= offset < self.byte_end


def load_registry(path: Path | None = None) -> list[Divergence]:
    p = path or DEFAULT_REGISTRY
    if not p.exists():
        return []
    data = json.loads(p.read_text(encoding="utf-8"))
    entries = [Divergence(**entry) for entry in data.get("divergences", [])]
    for d in entries:
        if d.fixture == "*" and d.target == "*":
            raise ValueError(
                f"registry entry {d.id} uses fixture='*' with target='*', "
                f"which would authorise a difference in any fixture on any "
                f"target. Name at least one of them.")
    return entries


def is_authorised(registry: list[Divergence], fixture: str, combo,
                  offset: int) -> Divergence | None:
    for d in registry:
        if d.covers(fixture, combo.target, offset):
            return d
    return None


def unauthorised_differences(registry: list[Divergence], fixture: str, combo,
                             expected: bytes, produced: bytes
                             ) -> list[tuple[int, int, str]]:
    """Every difference the registry does not cover, as (start, end, why).

    Empty means the comparison is fully authorised. Non-empty means
    failure, which is the registry contract: an unregistered difference
    is a failure, always.

    Two rules make that contract hold.

    Every differing run is checked, not only the first. Passing a file
    because its first differing byte happened to fall inside a registered
    range would let every later unauthorised difference through, which
    would quietly invert the contract.

    A length difference is never authorisable by a byte-range entry. A
    range says "these bytes may differ", not "the database may be a
    different size". A truncated or extended DDB is a structural fault
    that no byte range describes meaningfully. A fix that legitimately
    changes output length needs a different registry shape, not a range.
    """
    from .diffreport import differing_runs

    if len(expected) != len(produced):
        lo, hi = sorted((len(expected), len(produced)))
        return [(lo, hi,
                 f"length mismatch: expected {len(expected)} bytes, produced "
                 f"{len(produced)}; a byte-range entry cannot authorise a "
                 f"length change")]

    # An entry whose range runs past the end of the file it claims to describe
    # is a typo, not an authorisation. Catching it here matters because a
    # mis-scoped entry is otherwise indistinguishable from a correct one, and
    # it would silently authorise any real regression inside its range.
    for d in registry:
        if d.fixture != "*" and d.fixture != fixture:
            continue
        if d.target != "*" and d.target != combo.target:
            continue
        if d.byte_end > len(expected):
            return [(0, len(expected),
                     f"registry entry {d.id} covers bytes {d.byte_start}.."
                     f"{d.byte_end} but this output is only {len(expected)} "
                     f"bytes - the entry is mis-scoped and authorises nothing")]

    out: list[tuple[int, int, str]] = []
    for start, end in differing_runs(expected, produced):
        uncovered = [off for off in range(start, end)
                     if is_authorised(registry, fixture, combo, off) is None]
        if uncovered:
            out.append((start, end,
                        f"{len(uncovered)} of {end - start} byte(s) covered by "
                        f"no registry entry"))
    return out
