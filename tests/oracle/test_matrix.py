# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Matrix enumeration tests. Run directly: python test_matrix.py

Deliberately not a pytest suite. The check()/FAILURES pattern records
failures rather than raising, so under pytest every test would report as
passing regardless of outcome. main() is the only path that reports
correctly, and it is what CI runs.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from ndrcoracle.matrix import TARGETS, Combo, all_combos, nextdaad_combos

FAILURES = []


def check(cond, label):
    if not cond:
        FAILURES.append(label)


def test_target_count():
    check(len(TARGETS) == 14, f"expected 14 targets, got {len(TARGETS)}")


def test_subtarget_counts():
    check(len(TARGETS["ZX"]) == 6, "ZX must have 6 subtargets")
    check(len(TARGETS["MSX2"]) == 12, "MSX2 must have 12 subtargets")
    check(len(TARGETS["PC"]) == 5, "PC must have 5 subtargets")
    check(len(TARGETS["ZX81"]) == 2, "ZX81 must have 2 subtargets")
    check(TARGETS["NEXTDAAD"] == [None], "NEXTDAAD takes no subtarget")


def test_combination_total():
    combos = all_combos()
    # 35 target/subtarget pairs x EN/ES x v2/v3 x opt/classic
    check(len(combos) == 280, f"expected 280 combos, got {len(combos)}")


def test_slug_is_unique_and_filename_safe():
    combos = all_combos()
    slugs = [c.slug for c in combos]
    check(len(set(slugs)) == len(slugs), "slugs must be unique")
    bad = [s for s in slugs if not all(ch.isalnum() or ch == "_" for ch in s)]
    check(not bad, f"slugs must be filename-safe, offenders: {bad[:3]}")


def test_slug_shape():
    c = Combo(target="ZX", subtarget="PLUS3", lang="EN", v3=True, classic=False)
    check(c.slug == "ZX_PLUS3_EN_v3_opt", f"unexpected slug {c.slug}")
    c2 = Combo(target="CPC", subtarget=None, lang="ES", v3=False, classic=True)
    check(c2.slug == "CPC_ES_v2_classic", f"unexpected slug {c2.slug}")


def test_nextdaad_subset():
    combos = nextdaad_combos()
    check(len(combos) == 8, f"expected 8 NEXTDAAD combos, got {len(combos)}")
    check(all(c.target == "NEXTDAAD" for c in combos), "must all be NEXTDAAD")


def test_combo_flags_defaults_to_empty_tuple():
    c = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True, classic=False)
    check(c.flags == (), f"flags should default to (), got {c.flags!r}")
    check(c.slug == "NEXTDAAD_EN_v3_opt",
          f"a combo with no flags must keep the old slug shape, got {c.slug}")


def test_slug_single_flag_x():
    # RULING (ndrc-phase1c ledger, task-2): strip leading '-', drop '=',
    # lowercase, keep [a-z0-9] only.
    c = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
              classic=False, flags=("-x",))
    check(c.slug.endswith("_opt_x"),
          f"expected slug to end with _opt_x, got {c.slug}")
    check(c.slug == "NEXTDAAD_EN_v3_opt_x", f"unexpected slug {c.slug}")


def test_slug_single_flag_base_override():
    c = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
              classic=False, flags=("-b=0x9000",))
    check(c.slug.endswith("_b0x9000"),
          f"expected slug to end with _b0x9000, got {c.slug}")
    check(c.slug == "NEXTDAAD_EN_v3_opt_b0x9000", f"unexpected slug {c.slug}")


def test_slug_multiple_flags_join_with_underscore():
    c = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
              classic=False, flags=("-x", "-b=0x9000"))
    check(c.slug == "NEXTDAAD_EN_v3_opt_x_b0x9000", f"unexpected slug {c.slug}")


def test_slug_flags_p_and_d_examples_from_brief():
    c_p = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
                classic=False, flags=("-p",))
    check(c_p.slug == "NEXTDAAD_EN_v3_opt_p", f"unexpected slug {c_p.slug}")
    c_d = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
                classic=False, flags=("-d",))
    check(c_d.slug == "NEXTDAAD_EN_v3_opt_d", f"unexpected slug {c_d.slug}")


def test_slug_with_flags_stays_filename_safe():
    c = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
              classic=False, flags=("-b=0x9000",))
    check(all(ch.isalnum() or ch == "_" for ch in c.slug),
          f"slug with flags must be filename-safe, got {c.slug}")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}")
        print(f"matrix: {len(FAILURES)} failures")
        return 1
    print("matrix: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
