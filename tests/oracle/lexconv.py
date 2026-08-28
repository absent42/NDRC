# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Extracts the DAAD DFA lexer tables from the reference lexer.pas into
the committed C header src/front/lex_tables.h, plus a canonical text
dump tests/oracle/lex_tables.canon used to cross-check the emitted C
arrays against an independent regeneration (tests/test_lexconv_dump.c).

Design: parse lexer.pas's own generated DFA table region (7-bit ASCII,
between the literal markers "(* DFA table: *)" and "var yyn : Integer;"
- the nine pooled/per-state arrays TP Lex itself wrote there, plus the
four yyn* size constants), assert the invariants that region's own
structure guarantees (array lengths, the yyk/yym accept-table pooling,
the BOL/mid-line row-0/row-1 identity, cc-set disjointness within a
state, rule/state numbers in range), then emit a two-layer C
representation - a faithful struct-array transcription of yyt plus a
flattened [state][256] next-state matrix - and a canonical dump of the
flattened matrix plus each state's accept list, for cross-checking.

Two modes:

  python lexconv.py            "generate": locate the reference tree,
                                parse lexer.pas, validate every
                                invariant, run the round-trip
                                validation (see validate_round_trip),
                                then OVERWRITE src/front/lex_tables.h
                                and tests/oracle/lex_tables.canon.

  python lexconv.py --check    Verify without rewriting. Two arms,
                                chosen automatically:

                                - MAINTAINER ARM, when the reference
                                  tree is found (oracle.local.json's
                                  "drf" entry has a lexer.pas sibling -
                                  see find_reference_lexer_pas): re-runs
                                  the full generate() pipeline and
                                  byte-compares BOTH emitted files
                                  against the committed ones. This is
                                  the real gate against the Pascal
                                  source drifting out from under the
                                  committed header/canon.

                                - CANON-CONSISTENCY ARM, otherwise (as
                                  in CI, which has no DRC checkout):
                                  parses the COMMITTED src/front/
                                  lex_tables.h C header directly (not
                                  the Pascal - there is no Pascal here)
                                  and independently regenerates the
                                  canonical dump from its yynext/yyk/
                                  yykl/yykh arrays, comparing against
                                  the committed canon. This is a weaker
                                  guard - the Python header<->canon
                                  comparison alone is nearly circular
                                  since both come from the same parse -
                                  it only catches the header and canon
                                  drifting apart from each other post-
                                  commit, not a mistake present in both
                                  since the commit that added them. The
                                  compiled C checker
                                  (tests/test_lexconv_dump.c) is what
                                  closes that gap: it walks the
                                  emitted C arrays themselves (not this
                                  script's own Python model of them).

The reference tree, when present, is D:/DRC on git branch "nextdaad"
(project constraint: that branch is the ONLY authority) - generate()
and the maintainer arm both verify the branch before reading anything
from it, and refuse to run if it is not checked out to nextdaad.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from ndrcoracle.config import load_config, ConfigError

ROOT = Path(__file__).resolve().parents[2]
HEADER_PATH = ROOT / "src" / "front" / "lex_tables.h"
CANON_PATH = ROOT / "tests" / "oracle" / "lex_tables.canon"

TABLE_START_MARKER = "(* DFA table: *)"
TABLE_END_MARKER = "var yyn : Integer;"

# DSF.l has 43 rules; every yyk/yym entry must be a rule number in range.
RULE_MIN, RULE_MAX = 1, 43


# --------------------------------------------------------------------------
# Tokenizer - the Pascal fragment that appears inside the DFA table region.
# --------------------------------------------------------------------------

class Token:
    __slots__ = ("kind", "value", "pos")

    def __init__(self, kind: str, value, pos: int) -> None:
        self.kind = kind
        self.value = value
        self.pos = pos

    def __repr__(self) -> str:  # pragma: no cover - debugging aid only
        return f"Token({self.kind!r}, {self.value!r}, pos={self.pos})"


def tokenize(text: str) -> list[Token]:
    """Tokenizes one Pascal source fragment from the DFA table region.

    Recognises exactly the constructs that appear in lexer.pas's own DFA
    table region:
      - "{ N: }" state-boundary comments -> COMMENT, value=N (or None for
        a comment that is not of that shape - none occur here, but a
        stray one is metadata, not an error, so it is kept generic).
      - "#<digits>" ordinal literals -> HASH, value=int.
      - Pascal-quoted character literals, '' meaning a literal quote
        character (the doubled-quote escape) -> STR, value=decoded str.
      - ".." the range operator -> DOTDOT.
      - single-char punctuation ( ) [ ] , ; : = -> that character as kind.
      - bare decimal integers -> NUM, value=int.
      - identifiers -> IDENT, value=str.
    Anything else raises ValueError - an unrecognised construct in this
    region means the actual lexer.pas disagrees with what this parser
    expects, never something to paper over.
    """
    tokens: list[Token] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if c == "{":
            j = text.index("}", i)
            body = text[i + 1:j].strip()
            m = re.fullmatch(r"(\d+)\s*:", body)
            tokens.append(Token("COMMENT", int(m.group(1)) if m else None, i))
            i = j + 1
            continue
        if c == "#":
            j = i + 1
            while j < n and text[j].isdigit():
                j += 1
            if j == i + 1:
                raise ValueError(f"bad '#' literal at offset {i}")
            tokens.append(Token("HASH", int(text[i + 1:j]), i))
            i = j
            continue
        if c == "'":
            j = i + 1
            chars: list[str] = []
            while True:
                if j >= n:
                    raise ValueError(f"unterminated string literal at offset {i}")
                if text[j] == "'":
                    if j + 1 < n and text[j + 1] == "'":
                        chars.append("'")
                        j += 2
                        continue
                    j += 1
                    break
                chars.append(text[j])
                j += 1
            tokens.append(Token("STR", "".join(chars), i))
            i = j
            continue
        if text.startswith("..", i):
            tokens.append(Token("DOTDOT", None, i))
            i += 2
            continue
        if c in "()[],;:=":
            tokens.append(Token(c, None, i))
            i += 1
            continue
        if c.isdigit():
            j = i
            while j < n and text[j].isdigit():
                j += 1
            tokens.append(Token("NUM", int(text[i:j]), i))
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            tokens.append(Token("IDENT", text[i:j], i))
            i = j
            continue
        raise ValueError(f"unexpected character {c!r} at offset {i}")
    return tokens


# --------------------------------------------------------------------------
# Region / array extraction.
# --------------------------------------------------------------------------

def extract_region(text: str) -> str:
    """The DFA table region: between TABLE_START_MARKER and
    TABLE_END_MARKER, asserted 7-bit ASCII throughout."""
    start = text.index(TABLE_START_MARKER)
    end = text.index(TABLE_END_MARKER, start)
    region = text[start:end]
    for idx, ch in enumerate(region):
        if ord(ch) > 127:
            raise ValueError(
                f"non-ASCII character {ch!r} (ord {ord(ch)}) at region "
                f"offset {idx} - the DFA table region must be 7-bit ASCII")
    return region


def parse_constants(region: str) -> dict[str, int]:
    consts = {}
    for name in ("yynmarks", "yynmatches", "yyntrans", "yynstates"):
        m = re.search(rf"\b{name}\s*=\s*(\d+)\s*;", region)
        if not m:
            raise ValueError(f"constant {name!r} not found in DFA table region")
        consts[name] = int(m.group(1))
    return consts


def extract_array_body(region: str, name: str) -> str:
    """Raw text between array `name`'s own "= (" and its matching
    top-level ");", located by the array's own header line (so array
    order in the file does not matter). Bracket-depth-aware: '(' '['
    push, ')' ']' pop, but a quoted Pascal character literal (which may
    itself contain '(' ')' '[' ']', as several yyt cc-lists do - e.g.
    the quoted '(' and ')' at lexer.pas's state 0) is scanned and
    skipped whole first, so its contents never perturb the depth count.
    "{ N: }" comments are skipped the same way.
    """
    m = re.search(
        rf"\b{name}\s*:\s*array\s*\[[^\]]*\]\s*of\s*\w+\s*=\s*\(", region)
    if not m:
        raise ValueError(f"array {name!r} not found in DFA table region")
    n = len(region)
    i = m.end()  # just past the opening '('
    start = i
    depth = 1
    while depth > 0:
        c = region[i]
        if c == "'":
            i += 1
            while True:
                if region[i] == "'":
                    if i + 1 < n and region[i + 1] == "'":
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            continue
        if c == "{":
            i = region.index("}", i) + 1
            continue
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        i += 1
    end = i - 1  # index of the matching ')'
    return region[start:end]


def parse_int_array_with_comments(body: str) -> tuple[list[int], dict[int, int]]:
    """Parses a pooled/per-state integer array body. Returns (values,
    comment_pos) where comment_pos[state] is the 1-based position (into
    `values`) that state's own "{ state: }" comment claims - i.e. the
    count of values already parsed, plus one, at the moment the comment
    token is seen. Used both for the per-state kl/kh/ml/mh/tl/th rows
    (whose own comments are purely structural) and for the pooled yyk/
    yym arrays, where comment_pos feeds the cross-check against yykl/
    yytl: each array's own "{ N: }" comment must open the slice
    position the OTHER array's yykl[N]/yytl[N] independently claims."""
    tokens = tokenize(body)
    values: list[int] = []
    comment_pos: dict[int, int] = {}
    for t in tokens:
        if t.kind == "COMMENT":
            if t.value is not None:
                comment_pos[t.value] = len(values) + 1
            continue
        if t.kind == "NUM":
            values.append(t.value)
            continue
        if t.kind == ",":
            continue
        raise ValueError(f"unexpected token {t} in integer array body")
    return values, comment_pos


def parse_cc_element(tokens: list[Token], i: int) -> tuple[set[int], int]:
    """Parses one cc-list element at tokens[i]: an atom, or an
    atom..atom range. An atom is '#<digits>' or a Pascal-quoted
    character (single char after '' de-escaping). Returns
    (ordinal_set, next_index)."""
    def atom(i: int) -> tuple[int, int]:
        t = tokens[i]
        if t.kind == "HASH":
            return t.value, i + 1
        if t.kind == "STR":
            if len(t.value) != 1:
                raise ValueError(f"multi-character literal {t.value!r} unsupported")
            return ord(t.value), i + 1
        raise ValueError(f"expected a cc-list atom, got {t}")

    lo, i = atom(i)
    if i < len(tokens) and tokens[i].kind == "DOTDOT":
        i += 1
        hi, i = atom(i)
        if hi < lo:
            raise ValueError(f"cc-list range #{lo}..#{hi} has hi < lo")
        return set(range(lo, hi + 1)), i
    return {lo}, i


def parse_yyt_body(body: str) -> tuple[list[tuple[set[int], int]], dict[int, int]]:
    """Parses the yyt pooled transition-record array body into a list of
    (cc_set, target_state) entries in file order, plus comment_pos (see
    parse_int_array_with_comments) recording each state's own claimed
    1-based slice-start position into that list."""
    tokens = tokenize(body)
    entries: list[tuple[set[int], int]] = []
    comment_pos: dict[int, int] = {}
    i = 0
    n = len(tokens)
    while i < n:
        t = tokens[i]
        if t.kind == "COMMENT":
            if t.value is not None:
                comment_pos[t.value] = len(entries) + 1
            i += 1
            continue
        if t.kind == "(":
            i += 1
            if not (tokens[i].kind == "IDENT" and tokens[i].value == "cc"):
                raise ValueError(f"expected 'cc' at token {i}, got {tokens[i]}")
            i += 1
            if tokens[i].kind != ":":
                raise ValueError(f"expected ':' after 'cc' at token {i}")
            i += 1
            if tokens[i].kind != "[":
                raise ValueError(f"expected '[' after 'cc:' at token {i}")
            i += 1
            cc: set[int] = set()
            while tokens[i].kind != "]":
                elem, i = parse_cc_element(tokens, i)
                cc |= elem
                if tokens[i].kind == ",":
                    i += 1
            i += 1  # consume ']'
            if tokens[i].kind != ";":
                raise ValueError(f"expected ';' after cc-list at token {i}")
            i += 1
            if not (tokens[i].kind == "IDENT" and tokens[i].value == "s"):
                raise ValueError(f"expected 's' at token {i}, got {tokens[i]}")
            i += 1
            if tokens[i].kind != ":":
                raise ValueError(f"expected ':' after 's' at token {i}")
            i += 1
            if tokens[i].kind != "NUM":
                raise ValueError(f"expected target state number at token {i}")
            target = tokens[i].value
            i += 1
            if tokens[i].kind != ")":
                raise ValueError(f"expected ')' closing yyt entry at token {i}")
            i += 1
            if i < n and tokens[i].kind == ",":
                i += 1
            entries.append((cc, target))
            continue
        raise ValueError(f"unexpected token {t} in yyt body")
    return entries, comment_pos


# --------------------------------------------------------------------------
# Independent (regex-based) re-derivation, used only by the round-trip
# validation (validate_round_trip, below) - a second, deliberately
# separate code path from tokenize()/parse_yyt_body above, so the round
# trip proves something the main parser re-checking its own output could
# not.
# --------------------------------------------------------------------------

# One atom is '#<digits>' or a quoted single character; a range mixes
# either atom kind on either side (e.g. lexer.pas's own '{'..#192), so
# the range pattern below is two independent atom groups either side of
# '..', not a same-kind-only pairing.
_ATOM = r"(?:#(\d+)|'(.)')"
_ELEM_RANGE = re.compile(rf"^{_ATOM}\.\.{_ATOM}$")
_ELEM_SINGLE = re.compile(rf"^{_ATOM}$")


def _atom_value(hash_digits: str | None, quoted_char: str | None) -> int:
    return int(hash_digits) if hash_digits is not None else ord(quoted_char)


def independent_cc_from_raw(raw: str) -> set[int]:
    """Second, independent derivation of one cc-list element's ordinal
    set straight from its raw source text - regex-based, not tokenizer-
    based - used only by validate_round_trip. A divergence from the main
    parser's result on the same text means the parse lost or changed
    something - the whole point of the round trip is to prove it did
    not."""
    raw = raw.strip()
    # The doubled-quote atom '''' denotes the single-quote character
    # itself (ordinal 39) - substituted before the atom patterns below
    # so this stays a genuinely different code path from the main
    # tokenizer's own STR handling while covering the same construct.
    raw = raw.replace("''''", "#39")
    m = _ELEM_RANGE.match(raw)
    if m:
        lo = _atom_value(m.group(1), m.group(2))
        hi = _atom_value(m.group(3), m.group(4))
        if hi < lo:
            raise ValueError(f"independent_cc_from_raw: {raw!r} has hi < lo")
        return set(range(lo, hi + 1))
    m = _ELEM_SINGLE.match(raw)
    if m:
        return {_atom_value(m.group(1), m.group(2))}
    raise ValueError(f"independent_cc_from_raw: unrecognised element {raw!r}")


def split_top_level_commas(s: str) -> list[str]:
    """Splits a cc-list body on commas outside a quoted character
    literal (no element here nests brackets, so quote-awareness is the
    only thing this needs)."""
    parts: list[str] = []
    current: list[str] = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "'":
            current.append(c)
            i += 1
            while i < n:
                current.append(s[i])
                if s[i] == "'":
                    if i + 1 < n and s[i + 1] == "'":
                        current.append(s[i + 1])
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            continue
        if c == ",":
            parts.append("".join(current))
            current = []
            i += 1
            continue
        current.append(c)
        i += 1
    parts.append("".join(current))
    return parts


def raw_yyt_cc_lists(body: str) -> list[set[int]]:
    """Independent re-derivation of every yyt entry's cc set (in file
    order), scanning the ORIGINAL text directly with a "cc: [ ... ]"
    bracket search rather than through tokenize()/parse_yyt_body."""
    results: list[set[int]] = []
    i, n = 0, len(body)
    while True:
        j = body.find("[", i)
        if j == -1:
            break
        head = body[:j]
        if not re.search(r"cc\s*:\s*$", head):
            i = j + 1
            continue
        k = j + 1
        while True:
            c = body[k]
            if c == "'":
                k += 1
                while True:
                    if body[k] == "'":
                        if k + 1 < n and body[k + 1] == "'":
                            k += 2
                            continue
                        k += 1
                        break
                    k += 1
                continue
            if c == "]":
                break
            k += 1
        raw_list = body[j + 1:k]
        cc: set[int] = set()
        for elem in split_top_level_commas(raw_list):
            cc |= independent_cc_from_raw(elem)
        results.append(cc)
        i = k + 1
    return results


def canonical_cc_text(cc: set[int]) -> str:
    """Canonical text for one cc set: maximal ascending runs, each
    rendered '#lo' or '#lo..#hi', comma-separated - every element
    normalised the same way regardless of whether the source spelled it
    as a hash literal or a quoted character, so two cc sets built from
    differently-spelled but equal source text compare equal here."""
    if not cc:
        return ""
    ordinals = sorted(cc)
    runs: list[tuple[int, int]] = []
    start = prev = ordinals[0]
    for v in ordinals[1:]:
        if v == prev + 1:
            prev = v
            continue
        runs.append((start, prev))
        start = prev = v
    runs.append((start, prev))
    return ",".join(f"#{lo}" if lo == hi else f"#{lo}..#{hi}" for lo, hi in runs)


def validate_round_trip(lt: "LexTables", region: str) -> None:
    """Re-derives every yyt cc-list from the ORIGINAL text via the
    independent regex-based scan above, and compares its canonical form
    against the canonical form of what the main tokenizer/parser
    produced. Raises ValueError with every mismatch on any difference -
    proving the main parser lost or changed nothing, by an independent
    route rather than by re-checking its own output."""
    body = extract_array_body(region, "yyt")
    raw_ccs = raw_yyt_cc_lists(body)
    parsed_ccs = [cc for cc, _ in lt.yyt]
    if len(raw_ccs) != len(parsed_ccs):
        raise ValueError(
            f"round-trip: independent scan found {len(raw_ccs)} cc-lists, "
            f"main parser found {len(parsed_ccs)}")
    mismatches = []
    for idx, (raw_cc, parsed_cc) in enumerate(zip(raw_ccs, parsed_ccs)):
        raw_canon = canonical_cc_text(raw_cc)
        parsed_canon = canonical_cc_text(parsed_cc)
        if raw_canon != parsed_canon:
            mismatches.append(
                f"  yyt entry {idx}: independent={raw_canon!r} "
                f"parser={parsed_canon!r}")
    if mismatches:
        raise ValueError(
            "round-trip validation failed - main parser and the "
            "independent regex-based scan disagree:\n"
            + "\n".join(mismatches))


# --------------------------------------------------------------------------
# The parsed intermediate representation, and its structural invariants.
# --------------------------------------------------------------------------

@dataclass
class LexTables:
    yynmarks: int
    yynmatches: int
    yyntrans: int
    yynstates: int
    yyk: list[int]
    yym: list[int]
    yykl: list[int]
    yykh: list[int]
    yyml: list[int]
    yymh: list[int]
    yytl: list[int]
    yyth: list[int]
    yyt: list[tuple[set[int], int]]


def slice_1based(pooled: list, lo: int, hi: int) -> list:
    """pooled[lo..hi] under Pascal's 1-based, lo>hi-means-empty
    convention."""
    if lo > hi:
        return []
    return pooled[lo - 1:hi]


def parse_lexer_pas(text: str) -> LexTables:
    region = extract_region(text)
    consts = parse_constants(region)

    yyk_vals, yyk_cpos = parse_int_array_with_comments(
        extract_array_body(region, "yyk"))
    yym_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yym"))
    yykl_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yykl"))
    yykh_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yykh"))
    yyml_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yyml"))
    yymh_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yymh"))
    yytl_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yytl"))
    yyth_vals, _ = parse_int_array_with_comments(
        extract_array_body(region, "yyth"))
    yyt_entries, yyt_cpos = parse_yyt_body(extract_array_body(region, "yyt"))

    lt = LexTables(
        yynmarks=consts["yynmarks"], yynmatches=consts["yynmatches"],
        yyntrans=consts["yyntrans"], yynstates=consts["yynstates"],
        yyk=yyk_vals, yym=yym_vals,
        yykl=yykl_vals, yykh=yykh_vals,
        yyml=yyml_vals, yymh=yymh_vals,
        yytl=yytl_vals, yyth=yyth_vals,
        yyt=yyt_entries,
    )

    # Cross-check: the state-boundary comment inside the pooled yyk/yyt
    # arrays must open the slice position yykl/yytl independently
    # claims for that state.
    mismatches = []
    for state, pos in yyk_cpos.items():
        if state >= len(lt.yykl):
            mismatches.append(f"yyk comment for state {state} has no yykl row")
        elif lt.yykl[state] != pos:
            mismatches.append(
                f"yyk comment for state {state} claims position {pos}, "
                f"yykl[{state}]={lt.yykl[state]}")
    for state, pos in yyt_cpos.items():
        if state >= len(lt.yytl):
            mismatches.append(f"yyt comment for state {state} has no yytl row")
        elif lt.yytl[state] != pos:
            mismatches.append(
                f"yyt comment for state {state} claims position {pos}, "
                f"yytl[{state}]={lt.yytl[state]}")
    if mismatches:
        raise ValueError(
            "state-boundary comment cross-check failed:\n"
            + "\n".join(f"  - {m}" for m in mismatches))

    return lt


def validate_invariants(lt: LexTables) -> None:
    """Every structural invariant lexer.pas's own generated table
    satisfies: array lengths match the four yyn* constants; the yyk/yym
    accept table is pooled identically (yyk==yym, yykl==yyml,
    yykh==yymh); the BOL (state 0) and mid-line (state 1) rows are
    byte-identical, transitions and accepts; no transition fires on #0
    (the EOF sentinel); each state's own transitions are pairwise
    disjoint (a DFA partition); every rule number and target state is
    in range. Raises ValueError, with every violation listed, on any
    failure - these all hold for the real lexer.pas, so a failure here
    means either this parser or the actual source has diverged from
    that, not something to work around."""
    errors: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            errors.append(msg)

    check(len(lt.yyk) == lt.yynmarks,
          f"len(yyk)={len(lt.yyk)} != yynmarks={lt.yynmarks}")
    check(len(lt.yym) == lt.yynmatches,
          f"len(yym)={len(lt.yym)} != yynmatches={lt.yynmatches}")
    check(len(lt.yyt) == lt.yyntrans,
          f"len(yyt)={len(lt.yyt)} != yyntrans={lt.yyntrans}")
    for name, arr in (("yykl", lt.yykl), ("yykh", lt.yykh),
                      ("yyml", lt.yyml), ("yymh", lt.yymh),
                      ("yytl", lt.yytl), ("yyth", lt.yyth)):
        check(len(arr) == lt.yynstates,
              f"len({name})={len(arr)} != yynstates={lt.yynstates}")

    if not errors:  # the slicing below assumes the lengths above hold
        check(lt.yyk == lt.yym, "yyk != yym (accept-table pooling)")
        check(lt.yykl == lt.yyml, "yykl != yyml (accept-table pooling)")
        check(lt.yykh == lt.yymh, "yykh != yymh (accept-table pooling)")

        if lt.yynstates >= 2:
            row0_accept = slice_1based(lt.yyk, lt.yykl[0], lt.yykh[0])
            row1_accept = slice_1based(lt.yyk, lt.yykl[1], lt.yykh[1])
            check(row0_accept == row1_accept,
                  f"state 0 accept list {row0_accept} != "
                  f"state 1 accept list {row1_accept}")
            row0_trans = slice_1based(lt.yyt, lt.yytl[0], lt.yyth[0])
            row1_trans = slice_1based(lt.yyt, lt.yytl[1], lt.yyth[1])
            check(row0_trans == row1_trans,
                  "state 0 transitions != state 1 transitions (BOL rows)")

        for cc, target in lt.yyt:
            check(0 not in cc, f"transition to state {target} has #0 in cc")
            check(0 <= target < lt.yynstates,
                  f"transition target {target} out of range [0,{lt.yynstates})")

        for state in range(lt.yynstates):
            entries = slice_1based(lt.yyt, lt.yytl[state], lt.yyth[state])
            seen: set[int] = set()
            for cc, _ in entries:
                overlap = seen & cc
                check(not overlap,
                      f"state {state} yyt cc sets overlap at "
                      f"{sorted(overlap)[:5]}")
                seen |= cc

        for r in lt.yyk:
            check(RULE_MIN <= r <= RULE_MAX,
                  f"rule number {r} out of range [{RULE_MIN},{RULE_MAX}]")

    if errors:
        raise ValueError(
            "invariant check failed:\n"
            + "\n".join(f"  - {e}" for e in errors))


# --------------------------------------------------------------------------
# 0-based conversion, the flattened layer, and the canonical dump.
# --------------------------------------------------------------------------

def shift_minus_one(values: list[int]) -> list[int]:
    """Pascal 1-based pooled index -> 0-based C index, preserving the
    lo>hi=empty convention (lo>hi iff (lo-1)>(hi-1))."""
    return [v - 1 for v in values]


def build_flattened(lt: LexTables) -> list[list[int]]:
    """The [yynstates][256] next-state "flattened layer" matrix: -1
    where unset, column 0 always -1 (no #0 in any cc set - the EOF
    sentinel). Asserts no cell is set twice - a state's own cc sets are
    pairwise disjoint (a DFA partition), which is exactly what makes
    this order-free flattening valid; a collision here means that
    invariant did not actually hold."""
    next_states = [[-1] * 256 for _ in range(lt.yynstates)]
    for state in range(lt.yynstates):
        entries = slice_1based(lt.yyt, lt.yytl[state], lt.yyth[state])
        for cc, target in entries:
            for ordv in cc:
                if next_states[state][ordv] != -1:
                    raise ValueError(
                        f"state {state}: ordinal {ordv} set twice while "
                        f"flattening (cc sets not disjoint)")
                next_states[state][ordv] = target
    return next_states


def build_accepts(lt: LexTables) -> list[list[int]]:
    """Each state's accept list (rule numbers), ascending."""
    return [sorted(slice_1based(lt.yyk, lt.yykl[s], lt.yykh[s]))
            for s in range(lt.yynstates)]


def render_canon(next_states: list[list[int]], accepts: list[list[int]]) -> bytes:
    """The canonical dump used to cross-check the emitted C arrays: one
    line per state, "STATE: n0 n1 ... n255 | a0 a1 ...", ASCII,
    LF-terminated. tests/test_lexconv_dump.c reproduces this exact join
    logic (a single space between every token, nothing trailing when
    the accept list is empty) from the emitted C arrays."""
    lines = []
    for state, (row, acc) in enumerate(zip(next_states, accepts)):
        parts = [f"{state}:"]
        parts.extend(str(v) for v in row)
        parts.append("|")
        parts.extend(str(v) for v in acc)
        lines.append(" ".join(parts))
    return ("\n".join(lines) + "\n").encode("ascii")


# --------------------------------------------------------------------------
# C header emission.
# --------------------------------------------------------------------------

def format_int_array(name: str, size_macro: str, values: list[int],
                     per_line: int = 12) -> str:
    lines = [f"static const int16_t {name}[{size_macro}] = {{"]
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        sep = "," if i + per_line < len(values) else ""
        lines.append("  " + ", ".join(str(v) for v in chunk) + sep)
    lines.append("};")
    return "\n".join(lines)


def format_yynext(next_states: list[list[int]]) -> str:
    lines = ["static const int16_t yynext[YYNSTATES][256] = {"]
    for state, row in enumerate(next_states):
        lines.append(f"  {{ /* state {state} */")
        for i in range(0, 256, 16):
            chunk = row[i:i + 16]
            lines.append("    " + ", ".join(str(v) for v in chunk) + ",")
        lines.append("  },")
    lines.append("};")
    return "\n".join(lines)


def format_yyt(yyt: list[tuple[set[int], int]]) -> str:
    lines = ["static const YYTRec yyt[YYNTRANS] = {"]
    for cc, target in yyt:
        words = [0] * 8
        for ordv in cc:
            words[ordv // 32] |= 1 << (ordv % 32)
        words_str = ", ".join(f"0x{w:08X}" for w in words)
        lines.append(f"  {{ {{ {words_str} }}, {target} }},")
    lines.append("};")
    return "\n".join(lines)


def emit_header(lt: LexTables, source_commit: str) -> str:
    """The committed src/front/lex_tables.h. Pascal array names are
    preserved verbatim (yyk, yym, yykl, yykh, yyml, yymh, yytl, yyth,
    yyt); the four yyn* constants become uppercase YYN* macros (a C
    naming convention - the arrays are what carry the Pascal names
    through, not the constants). yynext is new: a flattened
    [state][256] next-state matrix with no Pascal counterpart, built
    from yyt/yytl/yyth by build_flattened for a faster port-lexer
    walk."""
    yykl0 = shift_minus_one(lt.yykl)
    yykh0 = shift_minus_one(lt.yykh)
    yyml0 = shift_minus_one(lt.yyml)
    yymh0 = shift_minus_one(lt.yymh)
    yytl0 = shift_minus_one(lt.yytl)
    yyth0 = shift_minus_one(lt.yyth)
    next_states = build_flattened(lt)

    parts = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/* src/front/lex_tables.h - Copyright (C) 2026 Dan Gibson. */",
        "/*",
        " * GENERATED FILE - DO NOT EDIT BY HAND.",
        " *",
        " * Generator: tests/oracle/lexconv.py",
        f" * Source: lexer.pas @ {source_commit} (D:/DRC, branch nextdaad).",
        " * Regenerate with: python tests/oracle/lexconv.py",
        " *",
        " * Table layout: lexer.pas's generated DFA tables (yyk, yym,",
        " * yykl, yykh, yyml, yymh, yytl, yyth, yyt) plus the four yyn*",
        " * size constants. Pooled index arrays (yykl/yykh/yyml/yymh/",
        " * yytl/yyth) are 0-based here; the Pascal source is 1-based",
        " * (each value below is the source value minus one). The",
        " * lo>hi = empty-slice convention is unchanged by that shift.",
        " * yynext is a flattened [state][256] next-state matrix with",
        " * no Pascal counterpart, built from yyt for a faster port-",
        " * lexer walk.",
        " */",
        "#ifndef NDRC_FRONT_LEX_TABLES_H",
        "#define NDRC_FRONT_LEX_TABLES_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define YYNMARKS {lt.yynmarks}",
        f"#define YYNMATCHES {lt.yynmatches}",
        f"#define YYNTRANS {lt.yyntrans}",
        f"#define YYNSTATES {lt.yynstates}",
        "",
        "typedef struct {",
        "    uint32_t cc[8];  /* 256-bit set, bit n = character n (Pascal YYTRec.cc) */",
        "    int16_t s;       /* target state (Pascal YYTRec.s) */",
        "} YYTRec;",
        "",
        format_int_array("yyk", "YYNMARKS", lt.yyk),
        "",
        format_int_array("yym", "YYNMATCHES", lt.yym),
        "",
        format_int_array("yykl", "YYNSTATES", yykl0),
        "",
        format_int_array("yykh", "YYNSTATES", yykh0),
        "",
        format_int_array("yyml", "YYNSTATES", yyml0),
        "",
        format_int_array("yymh", "YYNSTATES", yymh0),
        "",
        format_int_array("yytl", "YYNSTATES", yytl0),
        "",
        format_int_array("yyth", "YYNSTATES", yyth0),
        "",
        format_yyt(lt.yyt),
        "",
        format_yynext(next_states),
        "",
        "#endif /* NDRC_FRONT_LEX_TABLES_H */",
        "",
    ]
    return "\n".join(parts)


# --------------------------------------------------------------------------
# Reference-tree location and the generate() pipeline.
# --------------------------------------------------------------------------

def find_reference_lexer_pas() -> Path | None:
    """D:/DRC/src/lexer.pas, located via oracle.local.json's own "drf"
    entry (its sibling in the same src/ directory) rather than a
    hardcoded path, so this follows the same machine-local config as
    the rest of tests/oracle/. Returns None (never raises) when
    oracle.local.json is absent or lexer.pas is not where it points -
    both are the normal CI situation."""
    try:
        cfg = load_config()
    except ConfigError:
        return None
    candidate = cfg.drf.parent / "lexer.pas"
    return candidate if candidate.exists() else None


def verify_reference_branch(drc_root: Path) -> None:
    """Constraint Rule 0.1: D:/DRC on branch nextdaad ONLY is authority.
    Refuses to read from the reference tree at all when it is checked
    out to anything else."""
    result = subprocess.run(
        ["git", "-C", str(drc_root), "branch", "--show-current"],
        capture_output=True, text=True, check=True)
    branch = result.stdout.strip()
    if branch != "nextdaad":
        raise RuntimeError(
            f"{drc_root} is on branch {branch!r}, not 'nextdaad' - "
            f"refusing to extract from a non-authoritative tree")


def get_source_commit(lexer_pas: Path) -> str:
    """The fork commit that last touched lexer.pas (not HEAD - a
    pathspec-filtered `git log`), for the header's own top comment."""
    drc_root = lexer_pas.parent.parent
    result = subprocess.run(
        ["git", "-C", str(drc_root), "log", "-1", "--format=%h",
         "--", "src/lexer.pas"],
        capture_output=True, text=True, check=True)
    commit = result.stdout.strip()
    if not commit:
        raise RuntimeError(
            f"could not determine lexer.pas's source commit via "
            f"git log in {drc_root}")
    return commit


def generate() -> tuple[LexTables, str, bytes]:
    """Runs the full pipeline against the reference tree: parse,
    validate every invariant, run the round-trip validation, and emit
    both files' content (without writing anything). Returns
    (lt, header_text, canon_bytes)."""
    lexer_pas = find_reference_lexer_pas()
    if lexer_pas is None:
        raise RuntimeError(
            "reference tree not found - lexconv.py's generate mode needs "
            "D:/DRC/src/lexer.pas, located via oracle.local.json's own "
            "'drf' entry; copy oracle.local.json.example to "
            "oracle.local.json if this is a maintainer machine")
    verify_reference_branch(lexer_pas.parent.parent)

    text = lexer_pas.read_bytes().decode("latin-1")
    lt = parse_lexer_pas(text)
    validate_invariants(lt)
    validate_round_trip(lt, extract_region(text))

    commit = get_source_commit(lexer_pas)
    header_text = emit_header(lt, commit)
    next_states = build_flattened(lt)
    accepts = build_accepts(lt)
    canon_bytes = render_canon(next_states, accepts)
    return lt, header_text, canon_bytes


# --------------------------------------------------------------------------
# --check: the two arms.
# --------------------------------------------------------------------------

def _check_maintainer_arm(header_path: Path, canon_path: Path) -> list[str]:
    failures = []
    try:
        _, header_text, canon_bytes = generate()
    except Exception as e:
        return [f"maintainer arm: generate() failed: {e}"]

    committed_header = header_path.read_bytes() if header_path.exists() else None
    committed_canon = canon_path.read_bytes() if canon_path.exists() else None

    header_bytes = header_text.encode("ascii")
    if committed_header is None:
        failures.append(f"{header_path}: missing")
    elif committed_header != header_bytes:
        failures.append(
            f"{header_path}: does not match a fresh extraction from "
            f"lexer.pas ({len(committed_header)} bytes committed, "
            f"{len(header_bytes)} bytes freshly generated)")
    if committed_canon is None:
        failures.append(f"{canon_path}: missing")
    elif committed_canon != canon_bytes:
        failures.append(
            f"{canon_path}: does not match a fresh extraction from "
            f"lexer.pas ({len(committed_canon)} bytes committed, "
            f"{len(canon_bytes)} bytes freshly generated)")
    return failures


_C_ARRAY_RE_TMPL = r"\b{name}\s*\[[^=]*\]\s*=\s*\{{"


def parse_c_int_array(header_text: str, name: str) -> list[int]:
    """Flattens a 'static const int16_t NAME[...] = { ... };' or
    NAME[...][...] declaration (row-major) into an ordered list of
    ints, by locating NAME's own '{' and matching its brace depth, then
    regexing out every integer (comments stripped first). This is
    intentionally simple, not a general C parser - it only has to
    understand what emit_header's own formatting produces."""
    m = re.search(_C_ARRAY_RE_TMPL.format(name=re.escape(name)), header_text)
    if not m:
        raise ValueError(f"array {name!r} not found in header")
    start = m.end() - 1  # the opening '{'
    depth = 0
    i = start
    n = len(header_text)
    while True:
        c = header_text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                i += 1
                break
        i += 1
    body = header_text[start:i]
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.DOTALL)
    return [int(x) for x in re.findall(r"-?\d+", body)]


def _check_canon_consistency_arm(header_path: Path, canon_path: Path) -> list[str]:
    """Parses the COMMITTED C header directly (no Pascal involved) and
    independently regenerates the canonical dump from its yynext/yyk/
    yykl/yykh arrays, comparing against the committed canon."""
    if not header_path.exists():
        return [f"{header_path}: missing"]
    if not canon_path.exists():
        return [f"{canon_path}: missing"]

    header_text = header_path.read_text(encoding="ascii")
    try:
        yynstates = int(re.search(r"#define\s+YYNSTATES\s+(\d+)", header_text).group(1))
        flat_next = parse_c_int_array(header_text, "yynext")
        yyk = parse_c_int_array(header_text, "yyk")
        yykl = parse_c_int_array(header_text, "yykl")
        yykh = parse_c_int_array(header_text, "yykh")
    except Exception as e:
        return [f"failed to parse {header_path}: {e}"]

    if len(flat_next) != yynstates * 256:
        return [f"yynext has {len(flat_next)} entries, expected "
                f"{yynstates} * 256 = {yynstates * 256}"]
    next_states = [flat_next[i * 256:(i + 1) * 256] for i in range(yynstates)]

    accepts = []
    for state in range(yynstates):
        lo, hi = yykl[state], yykh[state]
        accepts.append(sorted(yyk[lo:hi + 1]) if lo <= hi else [])

    regenerated = render_canon(next_states, accepts)
    committed_canon = canon_path.read_bytes()
    if regenerated != committed_canon:
        return [f"{canon_path}: does not match a fresh regeneration from "
                f"the committed {header_path} "
                f"({len(committed_canon)} bytes committed, "
                f"{len(regenerated)} bytes regenerated)"]
    return []


def run_check(header_path: Path = HEADER_PATH,
              canon_path: Path = CANON_PATH,
              force_no_reference: bool = False) -> list[str]:
    """Runs --check: the maintainer arm when the reference tree is
    found, the canon-consistency arm otherwise. force_no_reference lets
    tests exercise the canon-consistency arm on a machine that does
    have the reference tree."""
    lexer_pas = None if force_no_reference else find_reference_lexer_pas()
    if lexer_pas is not None:
        return _check_maintainer_arm(header_path, canon_path)
    return _check_canon_consistency_arm(header_path, canon_path)


# --------------------------------------------------------------------------
# CLI.
# --------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="verify without rewriting (see module docstring "
                         "for the maintainer vs. canon-consistency arms)")
    args = ap.parse_args(argv)

    if args.check:
        failures = run_check()
        if failures:
            for f in failures:
                print(f"FAIL {f}")
            print(f"lexconv --check: {len(failures)} failure(s)")
            return 1
        print("lexconv --check: OK")
        return 0

    try:
        lt, header_text, canon_bytes = generate()
    except Exception as e:
        print(f"ERROR: {e}")
        return 2

    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    HEADER_PATH.write_bytes(header_text.encode("ascii"))
    CANON_PATH.parent.mkdir(parents=True, exist_ok=True)
    CANON_PATH.write_bytes(canon_bytes)
    print(f"wrote {HEADER_PATH} ({len(header_text)} bytes)")
    print(f"wrote {CANON_PATH} ({len(canon_bytes)} bytes)")
    print(f"{lt.yynstates} states, {lt.yyntrans} transitions, "
          f"{lt.yynmarks} accept marks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
