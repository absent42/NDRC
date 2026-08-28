# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Tests for lexconv.py.

Run directly: python test_lexconv.py
Not a pytest suite, for the reason given in test_matrix.py.

Deliberately does not depend on the reference tree (D:/DRC): CI has no
DRC checkout, so every parser/invariant/emission test here runs against
small, self-contained synthetic Pascal fixtures shaped like lexer.pas's
own generated DFA table region, not the real file. The --check
tamper-detection tests use the COMMITTED src/front/lex_tables.h and
tests/oracle/lex_tables.canon instead (always present, checked into
git), copied to a temp directory so nothing here writes to the real
committed files.
"""
from __future__ import annotations

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import lexconv
from lexconv import (
    tokenize, parse_constants, extract_region, extract_array_body,
    parse_int_array_with_comments, parse_cc_element, parse_yyt_body,
    parse_lexer_pas, validate_invariants, validate_round_trip,
    canonical_cc_text, independent_cc_from_raw, split_top_level_commas,
    build_flattened, build_accepts, render_canon, run_check,
    HEADER_PATH, CANON_PATH,
)

FAILURES = []


def check(cond, label):
    if not cond:
        FAILURES.append(label)


def check_raises(fn, label):
    try:
        fn()
    except (ValueError, RuntimeError):
        return
    FAILURES.append(f"{label}: expected an exception, none raised")


# --------------------------------------------------------------------------
# Synthetic fixtures - self-contained, no dependency on D:/DRC.
# --------------------------------------------------------------------------

# A minimal, internally-consistent 3-state table: states 0/1 are the
# BOL/mid-line pair (byte-identical, empty accept slices, one transition
# each on 'a' -> state 2); state 2 is dead and accepts rule 7.
SYNTH_VALID = """(* DFA table: *)

type YYTRec = record
                cc : set of Char;
                s  : Integer;
              end;

const

yynmarks   = 1;
yynmatches = 1;
yyntrans   = 2;
yynstates  = 3;

yyk : array [1..yynmarks] of Integer = (
  { 0: }
  { 1: }
  { 2: }
  7
);

yym : array [1..yynmatches] of Integer = (
{ 0: }
{ 1: }
{ 2: }
  7
);

yyt : array [1..yyntrans] of YYTrec = (
{ 0: }
  ( cc: [ 'a' ]; s: 2),
{ 1: }
  ( cc: [ 'a' ]; s: 2),
{ 2: }
);

yykl : array [0..yynstates-1] of Integer = (
{ 0: } 1,
{ 1: } 1,
{ 2: } 1
);

yykh : array [0..yynstates-1] of Integer = (
{ 0: } 0,
{ 1: } 0,
{ 2: } 1
);

yyml : array [0..yynstates-1] of Integer = (
{ 0: } 1,
{ 1: } 1,
{ 2: } 1
);

yymh : array [0..yynstates-1] of Integer = (
{ 0: } 0,
{ 1: } 0,
{ 2: } 1
);

yytl : array [0..yynstates-1] of Integer = (
{ 0: } 1,
{ 1: } 2,
{ 2: } 3
);

yyth : array [0..yynstates-1] of Integer = (
{ 0: } 1,
{ 1: } 2,
{ 2: } 2
);

var yyn : Integer;
"""

# state 0's transition targets state 2; state 1 (its BOL twin) targets
# state 3 instead - violates "state row 0 equals state row 1". No accept
# marks (yynmarks=0) - only the transition-row invariant is under test.
SYNTH_ROW_MISMATCH = """(* DFA table: *)

type YYTRec = record
                cc : set of Char;
                s  : Integer;
              end;

const

yynmarks   = 0;
yynmatches = 0;
yyntrans   = 2;
yynstates  = 4;

yyk : array [1..yynmarks] of Integer = (
);

yym : array [1..yynmatches] of Integer = (
);

yyt : array [1..yyntrans] of YYTrec = (
{ 0: }
  ( cc: [ 'a' ]; s: 2),
{ 1: }
  ( cc: [ 'a' ]; s: 3),
{ 2: }
{ 3: }
);

yykl : array [0..yynstates-1] of Integer = (
  1, 1, 1, 1
);

yykh : array [0..yynstates-1] of Integer = (
  0, 0, 0, 0
);

yyml : array [0..yynstates-1] of Integer = (
  1, 1, 1, 1
);

yymh : array [0..yynstates-1] of Integer = (
  0, 0, 0, 0
);

yytl : array [0..yynstates-1] of Integer = (
  1, 2, 3, 3
);

yyth : array [0..yynstates-1] of Integer = (
  1, 2, 2, 2
);

var yyn : Integer;
"""

# state 0 has two transitions both containing 'a' - violates the
# "cc sets pairwise disjoint" invariant. State 1 mirrors state 0 exactly
# so the row0==row1 invariant stays clean, isolating this one defect.
SYNTH_OVERLAP = """(* DFA table: *)

type YYTRec = record
                cc : set of Char;
                s  : Integer;
              end;

const

yynmarks   = 0;
yynmatches = 0;
yyntrans   = 4;
yynstates  = 3;

yyk : array [1..yynmarks] of Integer = (
);

yym : array [1..yynmatches] of Integer = (
);

yyt : array [1..yyntrans] of YYTrec = (
{ 0: }
  ( cc: [ 'a' ]; s: 2),
  ( cc: [ 'a'..'b' ]; s: 2),
{ 1: }
  ( cc: [ 'a' ]; s: 2),
  ( cc: [ 'a'..'b' ]; s: 2),
{ 2: }
);

yykl : array [0..yynstates-1] of Integer = (
  1, 1, 1
);

yykh : array [0..yynstates-1] of Integer = (
  0, 0, 0
);

yyml : array [0..yynstates-1] of Integer = (
  1, 1, 1
);

yymh : array [0..yynstates-1] of Integer = (
  0, 0, 0
);

yytl : array [0..yynstates-1] of Integer = (
  1, 3, 5
);

yyth : array [0..yynstates-1] of Integer = (
  2, 4, 4
);

var yyn : Integer;
"""

# yyk[0]=7, yym[0]=9 at the SAME pooled position (kl==ml, kh==mh) -
# violates "yyk == yym" (the accept-table pooling invariant) while
# keeping the index arrays themselves identical.
SYNTH_YYK_NE_YYM = """(* DFA table: *)

type YYTRec = record
                cc : set of Char;
                s  : Integer;
              end;

const

yynmarks   = 1;
yynmatches = 1;
yyntrans   = 0;
yynstates  = 3;

yyk : array [1..yynmarks] of Integer = (
  { 0: }
  { 1: }
  { 2: }
  7
);

yym : array [1..yynmatches] of Integer = (
{ 0: }
{ 1: }
{ 2: }
  9
);

yyt : array [1..yyntrans] of YYTrec = (
{ 0: }
{ 1: }
{ 2: }
);

yykl : array [0..yynstates-1] of Integer = (
  1, 1, 1
);

yykh : array [0..yynstates-1] of Integer = (
  0, 0, 1
);

yyml : array [0..yynstates-1] of Integer = (
  1, 1, 1
);

yymh : array [0..yynstates-1] of Integer = (
  0, 0, 1
);

yytl : array [0..yynstates-1] of Integer = (
  1, 1, 1
);

yyth : array [0..yynstates-1] of Integer = (
  0, 0, 0
);

var yyn : Integer;
"""


# --------------------------------------------------------------------------
# Tokenizer.
# --------------------------------------------------------------------------

def test_tokenize_hash_literal():
    toks = tokenize("#65")
    check(len(toks) == 1 and toks[0].kind == "HASH" and toks[0].value == 65,
          f"expected one HASH(65) token, got {toks}")


def test_tokenize_quoted_char():
    toks = tokenize("'a'")
    check(len(toks) == 1 and toks[0].kind == "STR" and toks[0].value == "a",
          f"expected one STR('a') token, got {toks}")


def test_tokenize_doubled_quote_is_a_literal_quote():
    toks = tokenize("''''")
    check(len(toks) == 1 and toks[0].kind == "STR" and toks[0].value == "'",
          f"expected one STR(\"'\") token for '''', got {toks}")


def test_tokenize_range_operator():
    toks = tokenize("#1..#5")
    kinds = [t.kind for t in toks]
    check(kinds == ["HASH", "DOTDOT", "HASH"],
          f"expected HASH DOTDOT HASH, got {kinds}")


def test_tokenize_state_comment():
    toks = tokenize("{ 12: }")
    check(len(toks) == 1 and toks[0].kind == "COMMENT" and toks[0].value == 12,
          f"expected one COMMENT(12) token, got {toks}")


def test_tokenize_rejects_unrecognised_character():
    check_raises(lambda: tokenize("~"), "tokenize('~') should raise")


# --------------------------------------------------------------------------
# cc-list element parsing (both the main parser and the independent one).
# --------------------------------------------------------------------------

def test_parse_cc_element_hash_range():
    toks = tokenize("#1..#5")
    cc, i = parse_cc_element(toks, 0)
    check(cc == set(range(1, 6)) and i == len(toks),
          f"expected {{1..5}}, got {cc}")


def test_parse_cc_element_mixed_atom_range():
    toks = tokenize("'{'..#192")
    cc, i = parse_cc_element(toks, 0)
    check(cc == set(range(ord("{"), 193)) and i == len(toks),
          f"expected range from '{{' to 192, got {sorted(cc)[:3]}...")


def test_independent_cc_from_raw_agrees_on_mixed_range():
    toks = tokenize("'{'..#192")
    parsed, _ = parse_cc_element(toks, 0)
    independent = independent_cc_from_raw("'{'..#192")
    check(parsed == independent,
          "independent_cc_from_raw must agree with the main parser on a "
          "mixed quoted-char/hash range")


def test_independent_cc_from_raw_doubled_quote():
    check(independent_cc_from_raw("''''") == {ord("'")},
          "independent_cc_from_raw('''') must be the quote ordinal (39)")


def test_split_top_level_commas_is_quote_aware():
    parts = split_top_level_commas("'(',')','+'")
    check(parts == ["'('", "')'", "'+'"],
          f"expected 3 quoted-paren/plus parts, got {parts}")


# --------------------------------------------------------------------------
# canonical_cc_text - maximal-run rendering.
# --------------------------------------------------------------------------

def test_canonical_cc_text_empty():
    check(canonical_cc_text(set()) == "", "empty set must render as ''")


def test_canonical_cc_text_single():
    check(canonical_cc_text({5}) == "#5", "singleton must render as '#5'")


def test_canonical_cc_text_maximal_runs():
    got = canonical_cc_text({1, 2, 3, 7, 9, 10})
    check(got == "#1..#3,#7,#9..#10", f"expected maximal runs, got {got!r}")


# --------------------------------------------------------------------------
# extract_region / extract_array_body / parse_constants.
# --------------------------------------------------------------------------

def test_extract_region_rejects_non_ascii():
    text = (lexconv.TABLE_START_MARKER + "\n\xe9\n"
            + lexconv.TABLE_END_MARKER)
    check_raises(lambda: extract_region(text),
                 "a non-ASCII byte inside the table region must raise")


def test_extract_array_body_skips_quoted_brackets():
    region = "yyt : array [1..2] of YYTrec = ( ( cc: [ '(',')' ]; s: 5) );"
    body = extract_array_body(region, "yyt")
    check("cc: [ '(',')' ]; s: 5)" in body,
          f"quoted parens must not truncate the array body, got {body!r}")


def test_parse_constants_reads_all_four():
    region = extract_region(SYNTH_VALID)
    consts = parse_constants(region)
    check(consts == {"yynmarks": 1, "yynmatches": 1, "yyntrans": 2,
                     "yynstates": 3},
          f"unexpected constants {consts}")


# --------------------------------------------------------------------------
# Full pipeline on the valid synthetic fixture.
# --------------------------------------------------------------------------

def test_valid_fixture_array_lengths_match_constants():
    lt = parse_lexer_pas(SYNTH_VALID)
    check(len(lt.yyk) == lt.yynmarks, "len(yyk) must equal yynmarks")
    check(len(lt.yym) == lt.yynmatches, "len(yym) must equal yynmatches")
    check(len(lt.yyt) == lt.yyntrans, "len(yyt) must equal yyntrans")
    for name, arr in (("yykl", lt.yykl), ("yykh", lt.yykh),
                      ("yyml", lt.yyml), ("yymh", lt.yymh),
                      ("yytl", lt.yytl), ("yyth", lt.yyth)):
        check(len(arr) == lt.yynstates, f"len({name}) must equal yynstates")


def test_valid_fixture_passes_invariants_and_round_trip():
    lt = parse_lexer_pas(SYNTH_VALID)
    try:
        validate_invariants(lt)
        validate_round_trip(lt, extract_region(SYNTH_VALID))
    except ValueError as e:
        FAILURES.append(f"valid fixture must pass validation, got: {e}")


def test_valid_fixture_flattened_and_accepts():
    lt = parse_lexer_pas(SYNTH_VALID)
    next_states = build_flattened(lt)
    accepts = build_accepts(lt)
    check(next_states[0][ord("a")] == 2,
          "state 0 must transition to state 2 on 'a'")
    check(next_states[0][0] == -1, "column 0 (EOF) must stay -1")
    check(all(v == -1 for i, v in enumerate(next_states[0]) if i != ord("a")),
          "every other column of state 0 must stay unset")
    check(next_states[0] == next_states[1],
          "BOL rows 0 and 1 must flatten identically")
    check(accepts == [[], [], [7]],
          f"expected accepts [[], [], [7]], got {accepts}")


def test_valid_fixture_canon_round_trips_through_render():
    lt = parse_lexer_pas(SYNTH_VALID)
    canon = render_canon(build_flattened(lt), build_accepts(lt))
    lines = canon.decode("ascii").splitlines()
    check(len(lines) == 3, f"expected 3 canon lines, got {len(lines)}")
    check(lines[2].endswith("| 7"), f"state 2's line must end '| 7', got {lines[2]!r}")
    check(lines[0].endswith(" |"), f"state 0's line must end with an empty "
                                    f"accept list ' |', got {lines[0]!r}")
    check(canon.endswith(b"\n"), "canon dump must end with a trailing newline")


# --------------------------------------------------------------------------
# Each section-27 invariant, individually violated.
# --------------------------------------------------------------------------

def test_row0_row1_mismatch_is_caught():
    lt = parse_lexer_pas(SYNTH_ROW_MISMATCH)
    check_raises(lambda: validate_invariants(lt),
                 "state0/state1 transition mismatch must fail validation")


def test_disjointness_violation_is_caught():
    lt = parse_lexer_pas(SYNTH_OVERLAP)
    check_raises(lambda: validate_invariants(lt),
                 "overlapping cc sets in one state must fail validation")
    check_raises(lambda: build_flattened(lt),
                 "flattening overlapping cc sets must also raise")


def test_yyk_yym_pooling_violation_is_caught():
    lt = parse_lexer_pas(SYNTH_YYK_NE_YYM)
    check_raises(lambda: validate_invariants(lt),
                 "yyk != yym must fail validation")


# --------------------------------------------------------------------------
# --check: tamper detection against the COMMITTED header/canon (temp
# copies only - never writes to the real committed files).
# --------------------------------------------------------------------------

def test_check_canon_consistency_passes_on_untampered_copy():
    if not (HEADER_PATH.exists() and CANON_PATH.exists()):
        return  # nothing committed yet to copy - covered elsewhere in CI
    with tempfile.TemporaryDirectory() as tmp:
        tmp_header = Path(tmp) / "lex_tables.h"
        tmp_canon = Path(tmp) / "lex_tables.canon"
        shutil.copyfile(HEADER_PATH, tmp_header)
        shutil.copyfile(CANON_PATH, tmp_canon)
        failures = run_check(header_path=tmp_header, canon_path=tmp_canon,
                             force_no_reference=True)
        check(failures == [],
              f"untampered copy must pass canon-consistency, got {failures}")


def test_check_canon_consistency_detects_tampered_header():
    if not (HEADER_PATH.exists() and CANON_PATH.exists()):
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp_header = Path(tmp) / "lex_tables.h"
        tmp_canon = Path(tmp) / "lex_tables.canon"
        shutil.copyfile(HEADER_PATH, tmp_header)
        shutil.copyfile(CANON_PATH, tmp_canon)

        # Tamper: flip one value inside the yynext array.
        text = tmp_header.read_text(encoding="ascii")
        tampered = text.replace("-1, 17, 17, 17, 17, 17, 17, 17, 17, 16, 15",
                                "-1, 99, 17, 17, 17, 17, 17, 17, 17, 16, 15",
                                1)
        check(tampered != text, "tamper substitution must have matched "
                                "something in the real committed header")
        tmp_header.write_text(tampered, encoding="ascii")

        failures = run_check(header_path=tmp_header, canon_path=tmp_canon,
                             force_no_reference=True)
        check(failures != [],
              "--check must exit nonzero (report failures) against a "
              "tampered header")


def test_check_reports_missing_files():
    with tempfile.TemporaryDirectory() as tmp:
        missing_header = Path(tmp) / "nope.h"
        missing_canon = Path(tmp) / "nope.canon"
        failures = run_check(header_path=missing_header,
                             canon_path=missing_canon,
                             force_no_reference=True)
        check(failures != [], "missing header/canon must be reported as failures")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}")
        print(f"lexconv: {len(FAILURES)} failures")
        return 1
    print("lexconv: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
