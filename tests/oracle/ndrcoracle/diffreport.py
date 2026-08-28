# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Byte-level diff reporting.

"Files differ" is useless when the artefact is a 50 KB binary. This
reports the offset of each differing run in both hex and decimal, with
surrounding context from both sides, so a failure points at a location a
developer can reason about.
"""
from __future__ import annotations


def first_difference(a: bytes, b: bytes) -> int | None:
    """Offset of the first differing byte, or None if equal."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return None


def differing_runs(a: bytes, b: bytes,
                   max_runs: int | None = None) -> list[tuple[int, int]]:
    """Contiguous (start, end) spans where the two differ, within the overlap.

    max_runs=None returns every run. The divergence check needs all of
    them: authorising a file on the strength of its first differing byte
    would mask every later difference in that file.
    """
    runs: list[tuple[int, int]] = []
    n = min(len(a), len(b))
    i = 0
    while i < n:
        if max_runs is not None and len(runs) >= max_runs:
            break
        if a[i] != b[i]:
            start = i
            while i < n and a[i] != b[i]:
                i += 1
            runs.append((start, i))
        else:
            i += 1
    return runs


def _hexdump(data: bytes, start: int, end: int) -> str:
    lo = max(0, start)
    hi = min(len(data), end)
    if lo >= hi:
        return "(past end)"
    return " ".join(f"{x:02x}" for x in data[lo:hi])


def _section_name(section_map: list[tuple[str, int]], addr: int) -> str:
    """Name of the section that contains addr.

    section_map is (name, start_addr) pairs; a section runs from its own
    start address up to the next entry's start address, and the last
    entry's section runs to the end of the file. Sorted defensively here
    rather than trusting the caller, since an out-of-order map would
    silently misattribute every run after the first swap.
    """
    ordered = sorted(section_map, key=lambda e: e[1])
    name = ordered[0][0]
    for nm, start in ordered:
        if start <= addr:
            name = nm
        else:
            break
    return name


def format_diff(a: bytes, b: bytes,
                label_a: str = "reference", label_b: str = "ndrc",
                context: int = 8, max_runs: int = 5,
                section_map: list[tuple[str, int]] | None = None) -> str:
    if a == b:
        return f"identical ({len(a)} bytes)"

    lines: list[str] = []

    if len(a) != len(b):
        lines.append(
            f"length mismatch: {label_a} {len(a)} bytes, "
            f"{label_b} {len(b)} bytes")

    for start, end in differing_runs(a, b, max_runs):
        lo = max(0, start - context)
        hi = min(min(len(a), len(b)), end + context)
        header = (f"offset 0x{start:04X} ({start}), "
                 f"{end - start} byte(s) differ")
        if section_map:
            header += f" in section {_section_name(section_map, start)}"
        lines.append(header)
        lines.append(f"  {label_a}: {_hexdump(a, lo, hi)}")
        lines.append(f"  {label_b}: {_hexdump(b, lo, hi)}")

    total = sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])
    if total > sum(e - s for s, e in differing_runs(a, b, max_runs)):
        lines.append(f"({total} differing bytes in total, runs truncated)")

    return "\n".join(lines)
