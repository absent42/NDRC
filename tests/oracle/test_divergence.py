# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Tests for the expected-divergence registry.

Run directly: python test_divergence.py
Not a pytest suite, for the reason given in test_matrix.py.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from ndrcoracle.divergence import Divergence, unauthorised_differences
from ndrcoracle.matrix import Combo

FAILURES = []
COMBO = Combo(target="NEXTDAAD", subtarget=None, lang="EN",
              v3=True, classic=False)


def check(cond, label):
    if not cond:
        FAILURES.append(label)


def entry(start, end, target="NEXTDAAD", fixture="*"):
    return Divergence(id="TEST", fixture=fixture, target=target,
                      byte_start=start, byte_end=end,
                      cause="test", ruling="test")


def test_identical_has_nothing_unauthorised():
    out = unauthorised_differences([], "BLANK_EN", COMBO, b"abcd", b"abcd")
    check(out == [], f"identical must be clean, got {out}")


def test_unregistered_difference_is_reported():
    out = unauthorised_differences([], "BLANK_EN", COMBO, b"abcd", b"abXd")
    check(len(out) == 1, f"expected one unauthorised run, got {out}")


def test_covered_difference_is_authorised():
    reg = [entry(2, 3)]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abXd")
    check(out == [], f"covered run must be authorised, got {out}")


def test_later_unregistered_run_is_not_masked():
    """The regression test for the whole point of this function.

    The first differing byte sits inside a registered range; a second,
    unregistered difference follows. Checking only the first offset would
    pass this file, which is the hole being closed.
    """
    reg = [entry(1, 2)]
    expected = b"abcdefgh"
    produced = b"aXcdeYgh"
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, expected, produced)
    check(len(out) == 1, f"later run must be reported, got {out}")
    if out:
        check(out[0][0] == 5, f"unauthorised run should start at 5, got {out[0][0]}")


def test_partially_covered_run_is_reported():
    reg = [entry(2, 3)]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abXY")
    check(len(out) == 1, f"partial cover must fail, got {out}")


def test_adjacent_entries_may_jointly_cover_a_run():
    reg = [entry(2, 3), entry(3, 4)]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abXY")
    check(out == [], f"adjacent entries should cover, got {out}")


def test_length_mismatch_is_never_authorisable():
    reg = [entry(0, 10_000)]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abcde")
    check(len(out) == 1, "length change must never be authorised")
    if out:
        check("length" in out[0][2].lower(),
              f"reason should mention length, got {out[0][2]}")


def test_entry_for_another_target_does_not_apply():
    reg = [entry(2, 3, target="ZX")]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abXd")
    check(len(out) == 1, "a ZX entry must not authorise a NEXTDAAD diff")


def test_entry_for_another_fixture_does_not_apply():
    reg = [entry(2, 3, fixture="STARTER")]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abXd")
    check(len(out) == 1, "a STARTER entry must not authorise a BLANK_EN diff")


def test_entry_past_end_of_file_is_rejected():
    reg = [entry(0, 10_000_000)]
    out = unauthorised_differences(reg, "BLANK_EN", COMBO, b"abcd", b"abXd")
    check(len(out) == 1, f"mis-scoped entry must not authorise, got {out}")
    if out:
        check("mis-scoped" in out[0][2],
              f"reason should say mis-scoped, got {out[0][2]}")


def test_both_wildcards_is_refused_at_load():
    import json, tempfile, os
    from pathlib import Path
    from ndrcoracle.divergence import load_registry
    payload = {"divergences": [{"id": "BAD", "fixture": "*", "target": "*",
                                "byte_start": 0, "byte_end": 4,
                                "cause": "test", "ruling": "test"}]}
    fd, name = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    Path(name).write_text(json.dumps(payload), encoding="utf-8")
    raised = False
    try:
        load_registry(Path(name))
    except ValueError:
        raised = True
    finally:
        os.unlink(name)
    check(raised, "both-wildcards entry must be refused at load")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}")
        print(f"divergence: {len(FAILURES)} failures")
        return 1
    print("divergence: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
