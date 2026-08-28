# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Tests for byte-diff reporting."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from ndrcoracle.config import layout_for
from ndrcoracle.diffreport import differing_runs, first_difference, format_diff
from verify import build_section_map, HEADER_PATCH_FIELDS

FAILURES = []


def check(cond, label):
    if not cond:
        FAILURES.append(label)


def test_identical_has_no_difference():
    check(first_difference(b"abc", b"abc") is None, "identical must be None")


def test_finds_first_differing_offset():
    check(first_difference(b"abcdef", b"abcXef") == 3, "offset must be 3")


def test_shorter_input_differs_at_its_end():
    check(first_difference(b"abc", b"abcd") == 3, "truncation offset must be 3")
    check(first_difference(b"abcd", b"abc") == 3, "extension offset must be 3")


def test_empty_inputs():
    check(first_difference(b"", b"") is None, "both empty must be None")
    check(first_difference(b"", b"a") == 0, "empty vs one byte must be 0")


def test_format_reports_offset_in_hex_and_decimal():
    out = format_diff(b"\x00\x01\x02\x03", b"\x00\x01\xFF\x03")
    check("0x0002" in out, f"hex offset missing from:\n{out}")
    check("2" in out, f"decimal offset missing from:\n{out}")


def test_format_shows_both_sides():
    out = format_diff(b"\xAA", b"\xBB", label_a="reference", label_b="ndrc")
    check("reference" in out, "label_a missing")
    check("ndrc" in out, "label_b missing")
    check("aa" in out.lower(), "reference byte missing")
    check("bb" in out.lower(), "ndrc byte missing")


def test_format_reports_length_mismatch():
    out = format_diff(b"\x01\x02", b"\x01\x02\x03")
    check("length" in out.lower(), f"length mismatch not reported:\n{out}")


def test_format_says_identical_when_equal():
    out = format_diff(b"same", b"same")
    check("identical" in out.lower(), f"expected identical, got:\n{out}")


def test_format_limits_number_of_runs():
    # Alternating bytes give many SEPARATE runs, which is what max_runs
    # truncation is about. A fixture where every byte differs yields one
    # long run and would pass this assertion trivially.
    a = bytes(64)
    b = bytes((0xFF if i % 2 == 0 else 0) for i in range(64))
    runs = differing_runs(a, b)
    check(len(runs) > 3, f"fixture must produce many runs, got {len(runs)}")
    out = format_diff(a, b, max_runs=2)
    check(out.count("offset") <= 3, f"too many runs reported:{chr(10)}{out}")


def test_format_annotates_section_from_map():
    section_map = [("header", 0), ("texts", 60), ("vocab", 91)]
    a = bytearray(100)
    b = bytearray(100)
    b[70] = 0xFF
    out = format_diff(bytes(a), bytes(b), section_map=section_map)
    check("in section texts" in out, f"expected 'in section texts':\n{out}")


def test_format_section_map_low_offset_is_header():
    section_map = [("header", 0), ("texts", 60), ("vocab", 91)]
    a = bytearray(100)
    b = bytearray(100)
    b[5] = 0xFF
    out = format_diff(bytes(a), bytes(b), section_map=section_map)
    check("in section header" in out, f"expected 'in section header':\n{out}")


def test_format_section_map_beyond_last_section():
    section_map = [("header", 0), ("texts", 60), ("vocab", 91)]
    a = bytearray(100)
    b = bytearray(100)
    b[95] = 0xFF
    out = format_diff(bytes(a), bytes(b), section_map=section_map)
    check("in section vocab" in out, f"expected 'in section vocab':\n{out}")


def test_format_without_section_map_has_no_annotation():
    out = format_diff(b"\x00\x01\x02\x03", b"\x00\x01\xFF\x03")
    check("in section" not in out, f"unexpected section annotation:\n{out}")


def _synthetic_ddb(base_address: int, big_endian: bool) -> bytes:
    """Same construction as test_reference.py's own _synthetic_ddb
    (duplicated rather than imported, to keep this file's fixtures self-
    contained per the module's own not-a-pytest-suite convention): every
    HEADER_PATCH_FIELDS offset holds an ABSOLUTE address
    base_address + (offset * 4), little- or big-endian per big_endian.
    Sized past the highest field's own file offset (offset * 4) plus
    context, so build_section_map's derived sections all land inside a
    real byte buffer that format_diff can be run against below - not
    just inspected as bare (name, offset) pairs.
    """
    max_offset = max(off for _, off in HEADER_PATCH_FIELDS)
    size = max_offset * 4 + 64
    buf = bytearray(size)
    endian = ">" if big_endian else "<"
    for _, offset in HEADER_PATCH_FIELDS:
        addr = base_address + offset * 4
        struct.pack_into(f"{endian}H", buf, offset, addr)
    return bytes(buf)


def test_format_section_map_diff_exactly_at_section_start_boundary():
    """T9 carry-over: a diff whose offset exactly equals a section's own
    start address must attribute to THAT section, not the one before it.
    Uses layout_for("ZX", "48K") (base 0x8400, per config.py) through
    build_section_map, so the boundary offsets are the ones a real ZX
    48K run would produce, not round test numbers."""
    base_address, big_endian = layout_for("ZX", "48K")
    golden = _synthetic_ddb(base_address, big_endian)
    section_map = build_section_map(
        golden, big_endian=big_endian, base_address=base_address)
    by_name = dict(section_map)

    vocab_start = by_name["vocabulary"]
    produced = bytearray(golden)
    produced[vocab_start] ^= 0xFF

    out = format_diff(golden, bytes(produced), section_map=section_map)
    check("in section vocabulary" in out,
          f"diff at the section's own start offset must attribute to "
          f"it, not the previous section:\n{out}")
    check("in section connections lookup" not in out,
          f"must not misattribute to the preceding section:\n{out}")


def test_format_section_map_diff_in_final_byte_of_last_section():
    """T9 carry-over: a diff in the very last byte of the buffer, inside
    whichever section build_section_map placed last, must still
    attribute correctly - the last section has no explicit upper bound
    other than the end of the file (diffreport._section_name's own
    contract). Same nonzero-base ZX 48K layout as the test above."""
    base_address, big_endian = layout_for("ZX", "48K")
    golden = _synthetic_ddb(base_address, big_endian)
    section_map = build_section_map(
        golden, big_endian=big_endian, base_address=base_address)
    last_name, last_start = max(section_map, key=lambda e: e[1])
    check(last_start < len(golden) - 1,
          "fixture must place the last section before the buffer's own "
          "final byte for this boundary to mean anything")

    produced = bytearray(golden)
    produced[-1] ^= 0xFF

    out = format_diff(golden, bytes(produced), section_map=section_map)
    check(f"in section {last_name}" in out,
          f"the buffer's final byte must attribute to the last section "
          f"({last_name}):\n{out}")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}")
        print(f"diffreport: {len(FAILURES)} failures")
        return 1
    print("diffreport: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
