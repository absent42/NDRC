# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""The DRC target matrix.

Values transcribed from the reference compiler, not from documentation:
  drf.pas:311-317  isValidSubTarget
  drb.php:1238-1247 isValidSubtarget
Both agree. Where a target appears in neither function it takes no
subtarget, and passing one is an error.
"""
from __future__ import annotations

from dataclasses import dataclass

TARGETS: dict[str, list[str | None]] = {
    "NEXTDAAD": [None],
    "ZX": ["48K", "128K", "PLUS3", "ESXDOS", "UNO", "NEXT"],
    "CPC": [None],
    "C64": [None],
    "CP4": [None],
    "CPM": [None],
    "MSX": [None],
    "MSX2": ["5_6", "5_8", "6_6", "6_8", "7_6", "7_8",
             "8_6", "8_8", "10_6", "10_8", "12_6", "12_8"],
    "ZX81": ["16K", "SD81B"],
    "PCW": [None],
    "PC": ["VGA256", "VGA", "EGA", "CGA", "TEXT"],
    "AMIGA": [None],
    "ST": [None],
    "HTML": [None],
}

LANGUAGES = ("EN", "ES")

# RULING (ndrc-phase1c ledger, task-2): per flag, strip the leading '-',
# drop '=', lowercase, keep [a-z0-9] only; multiple flags join with '_'.
# "-x" -> "x"; "-b=0x9000" -> "b0x9000".
_FLAG_SLUG_KEEP = "abcdefghijklmnopqrstuvwxyz0123456789"


def _flag_slug(flag: str) -> str:
    """One flag's slug fragment, per the RULING above."""
    s = flag[1:] if flag.startswith("-") else flag
    s = s.replace("=", "").lower()
    return "".join(ch for ch in s if ch in _FLAG_SLUG_KEEP)


@dataclass(frozen=True)
class Combo:
    target: str
    subtarget: str | None
    lang: str
    v3: bool
    classic: bool
    flags: tuple[str, ...] = ()

    @property
    def slug(self) -> str:
        parts = [self.target]
        if self.subtarget:
            parts.append(self.subtarget)
        parts.append(self.lang)
        parts.append("v3" if self.v3 else "v2")
        parts.append("classic" if self.classic else "opt")
        for flag in self.flags:
            parts.append(_flag_slug(flag))
        return "_".join(parts)

    @property
    def drf_args(self) -> list[str]:
        args = [self.target]
        if self.subtarget:
            args.append(self.subtarget)
        return args

    @property
    def drb_args(self) -> list[str]:
        args = [self.target]
        if self.subtarget:
            args.append(self.subtarget)
        args.append(self.lang)
        return args


def all_combos(
    targets: dict[str, list[str | None]] | None = None,
    languages: tuple[str, ...] = LANGUAGES,
    versions: tuple[bool, ...] = (False, True),
    modes: tuple[bool, ...] = (False, True),
) -> list[Combo]:
    """Every combination, in a stable order so slugs sort predictably."""
    src = TARGETS if targets is None else targets
    out: list[Combo] = []
    for target, subtargets in src.items():
        for sub in subtargets:
            for lang in languages:
                for v3 in versions:
                    for classic in modes:
                        out.append(Combo(target, sub, lang, v3, classic))
    return out


def nextdaad_combos() -> list[Combo]:
    """All eight axis combinations for the target NextDAAD actually ships."""
    return all_combos(targets={"NEXTDAAD": [None]})
