# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Locates the reference DRC toolchain.

Paths are machine-specific, so they live in oracle.local.json, which is
gitignored. oracle.local.json.example is committed as the template.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

DEFAULT_CONFIG_NAME = "oracle.local.json"

# getBaseAddressByTarget (drb.php:1284-1303) as a switch-case table, cases
# in source order. The $adventure->forcedBaseAddress override at drb.php:
# 1288 is adventure-state (set by a #forcedbaseaddress directive in the
# DSF), not reachable from a bare target/subtarget pair, and no committed
# fixture sets it, so it is out of scope here.
_BASE_ADDRESS_BY_TARGET: dict[str, int] = {
    "ZX": 0x8400,
    "MSX": 0x0100,
    "CPC": 0x2880,
    "PCW": 0x0100,
    "CPM": 0x2000,
    "CP4": 0x7080,
    "C64": 0x3880,
    "NEXTDAAD": 0x0000,
}

# isLittleEndianPlatform (drb.php:1310-1313). Kept under its own name here,
# not renamed to "big_endian" at the source: the misnomer is load-bearing.
# The value it returns is fed straight into writeWord (drb.php:67-79) as
# the "swap the two bytes before writing" flag - when littleEndian is
# TRUE, writeWord swaps, which writes the high byte first, i.e. actually
# emits BIG-endian words. So a target where isLittleEndianPlatform()
# returns True (ST, AMIGA) gets big-endian output; every other target
# gets the unswapped, genuinely little-endian order. layout_for()
# reproduces the swap decision under the correct name, big_endian.
def _is_little_endian_platform(target: str) -> bool:
    return target in ("ST", "AMIGA")


# Spec 6.1's drift guard (docs/dev/phase1b-design.md section 6.1): "base
# address and byte order come from a Python-side mirror of the targets
# table in ndrcoracle/config.py, asserted at import against the row count
# (35) so the two tables cannot drift silently." layout_for() itself
# branches on target name only (subtarget matters solely for ZX81), so
# the pair-count guard lives here instead: the 35 (target, subtarget)
# pairs below are transcribed in src/targets.c's own row order (1
# NEXTDAAD + 6 ZX + 2 ZX81 + 5 PC + 12 MSX2 + 9 bare-subtarget targets =
# 35 - see targets.c's own row comments for the same arithmetic). A
# future 36th C row lands here too, or this assertion fails loudly at
# import.
ALL_TARGET_SUBTARGET_PAIRS: tuple[tuple[str, str | None], ...] = (
    ("NEXTDAAD", None),
    ("ZX", "48K"), ("ZX", "128K"), ("ZX", "PLUS3"), ("ZX", "ESXDOS"),
    ("ZX", "NEXT"), ("ZX", "UNO"),
    ("ZX81", "16K"), ("ZX81", "SD81B"),
    ("PC", "VGA256"), ("PC", "VGA"), ("PC", "CGA"), ("PC", "EGA"), ("PC", "TEXT"),
    ("MSX2", "5_6"), ("MSX2", "5_8"), ("MSX2", "6_6"), ("MSX2", "6_8"),
    ("MSX2", "7_6"), ("MSX2", "7_8"), ("MSX2", "8_6"), ("MSX2", "8_8"),
    ("MSX2", "10_6"), ("MSX2", "10_8"), ("MSX2", "12_6"), ("MSX2", "12_8"),
    ("C64", None), ("CPC", None), ("CP4", None), ("CPM", None),
    ("MSX", None), ("PCW", None), ("AMIGA", None), ("ST", None),
    ("HTML", None),
)

assert len(ALL_TARGET_SUBTARGET_PAIRS) == 35, (
    "ndrcoracle.config.ALL_TARGET_SUBTARGET_PAIRS must mirror all 35 "
    "(target, subtarget) rows of src/targets.c (docs/dev/phase1b-design.md "
    f"section 6.1); found {len(ALL_TARGET_SUBTARGET_PAIRS)} - update this "
    "mirror (and the count) alongside any change to the C table"
)


def layout_for(target: str, subtarget: str | None = None,
               base_override: int | None = None) -> tuple[int, bool]:
    """(base_address, big_endian) for one target/subtarget pair.

    base_address ports getBaseAddressByTarget (drb.php:1284-1303): every
    target but ZX81 ignores subtarget and looks up a fixed value (0 for
    any target absent from the switch, matching its `default: return 0`
    at drb.php:1301). ZX81 is the one target whose base depends on
    subtarget (drb.php:1294-1296): 16K -> 0x0000, SD81B -> 0x8400.

    big_endian ports isLittleEndianPlatform (drb.php:1310-1313) - see
    _is_little_endian_platform's docstring for why the source name and
    the actual byte-order effect are opposites.

    base_override, when given, is returned as base_address verbatim
    instead of the table/ZX81 lookup - this is the DRB/ndrc -b=...
    command-line override (task-2-brief.md Interfaces), which the --
    from-json sweep parses out of a combo's own flags and passes through
    here so build_section_map's addressing matches what the run actually
    used. big_endian is still the target's own value; -b= overrides the
    base address only, never the byte order.
    """
    if target == "ZX81":
        base = 0x8400 if subtarget == "SD81B" else 0x0000
    else:
        base = _BASE_ADDRESS_BY_TARGET.get(target, 0)
    if base_override is not None:
        base = base_override
    return base, _is_little_endian_platform(target)


class ConfigError(RuntimeError):
    pass


@dataclass(frozen=True)
class OracleConfig:
    drf: Path
    php: Path
    drb: Path
    workdir: Path


def load_config(path: Path | None = None) -> OracleConfig:
    if path is None:
        path = Path(__file__).resolve().parent.parent / DEFAULT_CONFIG_NAME
    if not path.exists():
        raise ConfigError(
            f"{path} not found. Copy oracle.local.json.example to "
            f"{DEFAULT_CONFIG_NAME} and set the paths for this machine."
        )
    data = json.loads(path.read_text(encoding="utf-8"))

    missing = [k for k in ("drf", "php", "drb") if k not in data]
    if missing:
        raise ConfigError(f"{path} is missing keys: {', '.join(missing)}")

    cfg = OracleConfig(
        drf=Path(data["drf"]),
        php=Path(data["php"]),
        drb=Path(data["drb"]),
        workdir=Path(data.get("workdir",
                              Path(__file__).resolve().parents[2] / "work")),
    )

    for label, p in (("drf", cfg.drf), ("php", cfg.php), ("drb", cfg.drb)):
        if not p.exists():
            raise ConfigError(f"{label} not found at {p} (from {path})")

    cfg.workdir.mkdir(parents=True, exist_ok=True)
    return cfg
