# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Tests for ndrcoracle/reference.py's run_reference(), against fakes.

Run directly: python test_reference.py
Not a pytest suite, for the reason given in test_matrix.py.

reference.py invokes cfg.drf and cfg.php directly as argv[0] (no shell),
so the fakes need a platform-appropriate executable in that slot: CI's
oracle job runs on ubuntu-latest while this may run on Windows, and
Windows cannot exec a bare python file. The fix is a generated shim per
platform, built by this test at runtime into a temp dir, wrapping the
committed pure-python payloads (fakes/fake_drf.py, fakes/fake_php.py):

  Windows  a .cmd that calls this interpreter on the payload, forwarding
           all args (subprocess.run can launch a .cmd directly here with
           no shell=True, confirmed empirically on this machine).
  POSIX    the payload's own source with a python shebang line prepended
           and the executable bit set, so the kernel's shebang handling
           runs it directly - no wrapper script needed.

Only the shims are generated; the fakes themselves are the two committed
files, imported here too so the fixed payloads (FIXED_JSON, FIXED_DDB)
are asserted against, not duplicated.

Fixture behaviour (ok/fail/noddb/sleep) is selected through the
FAKE_DRF_MODE / FAKE_PHP_MODE environment variables, which subprocess.run
inherits from this process - see fakes/fake_drf.py's docstring for why
argv could not carry it instead.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import stat
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
FAKES_DIR = Path(__file__).parent / "fakes"
sys.path.insert(0, str(FAKES_DIR))

import ndrcoracle.reference as reference
from ndrcoracle.config import ALL_TARGET_SUBTARGET_PAIRS, OracleConfig, layout_for
from ndrcoracle.matrix import Combo
import struct

import gen_goldens
import verify
from verify import (
    _pair_selected, _normalise_newlines, _strip_banner_line,
    build_section_map, HEADER_PATCH_FIELDS,
)
import fake_drf
import fake_php

FAILURES = []
COMBO = Combo(target="NEXTDAAD", subtarget=None, lang="EN",
              v3=True, classic=False)


def check(cond, label):
    if not cond:
        FAILURES.append(label)


def _make_shim(payload: Path, shim_dir: Path) -> Path:
    """Builds a platform-appropriate executable shim wrapping `payload`."""
    if os.name == "nt":
        shim = shim_dir / (payload.stem + ".cmd")
        shim.write_text(
            f'@echo off\r\n"{sys.executable}" "{payload}" %*\r\n',
            encoding="utf-8")
        return shim
    shim = shim_dir / payload.stem
    body = payload.read_text(encoding="utf-8")
    shim.write_text(f"#!/usr/bin/env python3\n{body}", encoding="utf-8")
    mode = shim.stat().st_mode
    shim.chmod(mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return shim


def _make_cfg(tmp_root: Path) -> OracleConfig:
    """A fresh OracleConfig with shimmed drf/php and a private workdir.

    Built directly rather than through config.load_config(): the loader's
    job is finding oracle.local.json on disk and checking ITS paths exist,
    neither of which applies to per-run generated shims, and OracleConfig
    itself is a plain dataclass with no validation of its own - nothing
    stops constructing it directly with test-controlled paths.
    """
    shim_dir = tmp_root / "shims"
    shim_dir.mkdir()
    workdir = tmp_root / "work"
    workdir.mkdir()
    drf = _make_shim(FAKES_DIR / "fake_drf.py", shim_dir)
    php = _make_shim(FAKES_DIR / "fake_php.py", shim_dir)
    return OracleConfig(drf=drf, php=php, drb=Path("unused_drb.php"),
                        workdir=workdir)


@contextmanager
def env_modes(drf_mode: str | None = None, php_mode: str | None = None):
    keys = {"FAKE_DRF_MODE": drf_mode, "FAKE_PHP_MODE": php_mode}
    old = {k: os.environ.get(k) for k in keys}
    try:
        for k, v in keys.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        yield
    finally:
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


@contextmanager
def _run(drf_mode: str | None = None, php_mode: str | None = None):
    """Yields (result, workdir); tmp_root (the mkdtemp above workdir) is
    always removed on exit, success or failure alike - M8. This is
    orthogonal to whether run_reference itself keeps or removes its own
    inner run dir under workdir on failure (asserted below via
    _run_dir_count), which is SUT behaviour, not test scaffolding."""
    tmp_root = Path(tempfile.mkdtemp(prefix="ndrc_ref_test_"))
    try:
        cfg = _make_cfg(tmp_root)
        dsf = tmp_root / "input.dsf"
        dsf.write_text("dummy dsf source\n", encoding="utf-8")
        with env_modes(drf_mode, php_mode):
            result = reference.run_reference(cfg, dsf, COMBO)
        yield result, cfg.workdir
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


@contextmanager
def _run_from_json(drf_mode: str | None = None, php_mode: str | None = None):
    """Same contract as _run above, for run_reference_from_json."""
    tmp_root = Path(tempfile.mkdtemp(prefix="ndrc_ref_fromjson_test_"))
    try:
        cfg = _make_cfg(tmp_root)
        dsf = tmp_root / "input.dsf"
        dsf.write_text("dummy dsf source\n", encoding="utf-8")
        with env_modes(drf_mode, php_mode):
            result = reference.run_reference_from_json(cfg, dsf, COMBO)
        yield result, cfg.workdir
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


def _run_dir_count(workdir: Path) -> int:
    return len(list(workdir.iterdir()))


def test_ok_path_returns_ddb_and_removes_run_dir():
    with _run() as (result, workdir):
        check(result.ok,
              f"expected ok, got stage={result.stage} stderr={result.stderr!r}")
        check(result.stage == "done", f"expected stage done, got {result.stage}")
        check(result.ddb == fake_php.FIXED_DDB,
              "ddb should match the fixed fake_php payload")
        check(result.json_text is not None and "fake" in result.json_text,
              "json_text should carry the fake_drf output")
        check(_run_dir_count(workdir) == 0, "run dir must be removed on success")


def test_drf_failure_returns_stage_drf_and_keeps_dir():
    with _run(drf_mode="fail") as (result, workdir):
        check(not result.ok, "drf failure must not be ok")
        check(result.stage == "drf", f"expected stage drf, got {result.stage}")
        check(result.ddb is None, "no ddb on drf failure")
        check(_run_dir_count(workdir) == 1, "run dir must be kept on drf failure")


def test_drb_failure_returns_stage_drb_and_keeps_dir():
    with _run(php_mode="fail") as (result, workdir):
        check(not result.ok, "drb failure must not be ok")
        check(result.stage == "drb", f"expected stage drb, got {result.stage}")
        check(result.ddb is None, "no ddb on drb failure")
        check(result.json_text is not None,
              "json_text from the completed drf stage should survive")
        check(_run_dir_count(workdir) == 1, "run dir must be kept on drb failure")


def test_missing_ddb_returns_stage_drb_and_keeps_dir():
    with _run(php_mode="noddb") as (result, workdir):
        check(not result.ok, "missing ddb must not be ok")
        check(result.stage == "drb", f"expected stage drb, got {result.stage}")
        check(result.ddb is None, "no ddb when drb wrote none")
        check("DDB" in result.stderr,
              f"stderr should mention the missing DDB, got {result.stderr!r}")
        check(_run_dir_count(workdir) == 1,
              "run dir must be kept when no DDB is produced")


def test_drf_timeout_converts_to_failed_result_stage_drf():
    old_timeout = reference.TIMEOUT_SECONDS
    reference.TIMEOUT_SECONDS = 2
    try:
        with _run(drf_mode="sleep") as (result, workdir):
            check(not result.ok, "drf timeout must not be ok")
            check(result.stage == "drf", f"expected stage drf, got {result.stage}")
            check("timed out" in result.stderr,
                  f"stderr should mention timeout, got {result.stderr!r}")
            check(_run_dir_count(workdir) == 1, "run dir must be kept on drf timeout")
    finally:
        reference.TIMEOUT_SECONDS = old_timeout


def test_drb_timeout_converts_to_failed_result_stage_drb():
    old_timeout = reference.TIMEOUT_SECONDS
    reference.TIMEOUT_SECONDS = 2
    try:
        with _run(php_mode="sleep") as (result, workdir):
            check(not result.ok, "drb timeout must not be ok")
            check(result.stage == "drb", f"expected stage drb, got {result.stage}")
            check("timed out" in result.stderr,
                  f"stderr should mention timeout, got {result.stderr!r}")
            check(result.json_text is not None,
                  "json_text from the completed drf stage should survive a drb timeout")
            check(_run_dir_count(workdir) == 1, "run dir must be kept on drb timeout")
    finally:
        reference.TIMEOUT_SECONDS = old_timeout


def test_layout_for_matches_drb_php():
    """(base_address, big_endian) per target/subtarget, values transcribed
    from getBaseAddressByTarget (drb.php:1284-1303) and
    isLittleEndianPlatform (drb.php:1310-1313) - see config.py's docstrings
    for the isLittleEndianPlatform naming/behaviour inversion.
    """
    cases = [
        (("ZX", None), (0x8400, False)),
        (("ZX", "48K"), (0x8400, False)),
        (("ZX81", "SD81B"), (0x8400, False)),
        (("ZX81", "16K"), (0x0000, False)),
        (("ST", None), (0x0000, True)),
        (("AMIGA", None), (0x0000, True)),
        (("MSX", None), (0x0100, False)),
        (("CPC", None), (0x2880, False)),
        (("CP4", None), (0x7080, False)),
        (("C64", None), (0x3880, False)),
        (("CPM", None), (0x2000, False)),
        (("PCW", None), (0x0100, False)),
        (("PC", "VGA256"), (0x0000, False)),
        (("HTML", None), (0x0000, False)),
        (("MSX2", "5_6"), (0x0000, False)),
        (("NEXTDAAD", None), (0x0000, False)),
    ]
    for (target, subtarget), expected in cases:
        got = layout_for(target, subtarget)
        check(got == expected,
              f"layout_for({target!r}, {subtarget!r}) = {got}, "
              f"expected {expected}")


def test_all_target_subtarget_pairs_guard_covers_35_and_layout_for_accepts_each():
    """Spec 6.1's drift guard (docs/dev/phase1b-design.md section 6.1): config.py
    asserts ALL_TARGET_SUBTARGET_PAIRS has exactly 35 entries at import time
    (already proven just by this module importing cleanly), and every pair
    in it must resolve through layout_for() without raising - a 36th pair
    added here without a real target would still import fine but this loop
    would have nothing to catch it either, so the length check is the
    guard's real teeth."""
    check(len(ALL_TARGET_SUBTARGET_PAIRS) == 35,
          f"expected 35 (target, subtarget) pairs, got "
          f"{len(ALL_TARGET_SUBTARGET_PAIRS)}")
    check(len(set(ALL_TARGET_SUBTARGET_PAIRS)) == 35,
          "ALL_TARGET_SUBTARGET_PAIRS contains a duplicate pair")
    for target, subtarget in ALL_TARGET_SUBTARGET_PAIRS:
        got = layout_for(target, subtarget)
        check(isinstance(got, tuple) and len(got) == 2,
              f"layout_for({target!r}, {subtarget!r}) = {got!r}, "
              f"expected a (base_address, big_endian) pair")


def test_pair_selected_only_filtering():
    """verify.py's --only: repeatable, AND semantics against "fixture/slug"
    (verify.py's _pair_selected, consumed by from_json_check's sweep)."""
    check(_pair_selected("BLANK_EN", "NEXTDAAD_EN_v3_opt", []),
          "no --only given should select every pair")
    check(_pair_selected("BLANK_EN", "NEXTDAAD_EN_v3_opt", ["BLANK_EN"]),
          "a substring present in the fixture half should select")
    check(_pair_selected("BLANK_EN", "NEXTDAAD_EN_v3_opt", ["v3_opt"]),
          "a substring present in the slug half should select")
    check(_pair_selected("BLANK_EN", "NEXTDAAD_EN_v3_opt",
                         ["BLANK_EN", "v3_opt"]),
          "two substrings that both match (AND) should select")
    check(not _pair_selected("BLANK_EN", "NEXTDAAD_EN_v3_opt",
                             ["BLANK_EN", "ZX"]),
          "one matching and one non-matching substring (AND) must not select")
    check(not _pair_selected("BLANK_EN", "NEXTDAAD_EN_v3_opt", ["STARTER"]),
          "a substring present in neither half must not select")
    check(not _pair_selected("STARTER", "NEXTDAAD_EN_v3_classic", ["opt"]),
          "'opt' is not a substring of '...v3_classic' - a classic slug "
          "must not be selected by an --only meant for opt-mode pairs")


def _synthetic_ddb(base_address: int, big_endian: bool) -> bytes:
    """A minimal buffer with a real header-patch-word layout: every
    HEADER_PATCH_FIELDS offset holds an ABSOLUTE address base_address +
    (offset * 4), written in the target's own byte order - large enough
    that build_section_map's arithmetic (addr - base_address) has
    something non-trivial, in each field's own order, to recover.
    """
    size = max(off for _, off in HEADER_PATCH_FIELDS) + 32
    buf = bytearray(size)
    endian = ">" if big_endian else "<"
    for _, offset in HEADER_PATCH_FIELDS:
        addr = base_address + offset * 4
        struct.pack_into(f"{endian}H", buf, offset, addr)
    return bytes(buf)


def test_build_section_map_nonzero_base_little_endian():
    """ZX-shaped layout: base 0x8400, little-endian - the guard
    build_section_map used to raise on for a non-zero base_address."""
    ddb = _synthetic_ddb(base_address=0x8400, big_endian=False)
    section_map = build_section_map(ddb, big_endian=False, base_address=0x8400)
    check(section_map[0] == ("header", 0),
          f"first entry should be the synthetic header entry, got {section_map[0]}")
    by_name = dict(section_map)
    for name, offset in HEADER_PATCH_FIELDS:
        expected_file_offset = offset * 4
        check(by_name[name] == expected_file_offset,
              f"{name}: got file offset {by_name[name]}, "
              f"expected {expected_file_offset}")
    check([addr for _, addr in section_map] == sorted(addr for _, addr in section_map),
          "section_map must be sorted by file offset")


def test_build_section_map_big_endian():
    """ST/AMIGA-shaped layout: base 0, big-endian words - the guard
    build_section_map used to raise on for big_endian=True."""
    ddb = _synthetic_ddb(base_address=0, big_endian=True)
    section_map = build_section_map(ddb, big_endian=True, base_address=0)
    by_name = dict(section_map)
    for name, offset in HEADER_PATCH_FIELDS:
        expected_file_offset = offset * 4
        check(by_name[name] == expected_file_offset,
              f"{name}: got file offset {by_name[name]}, "
              f"expected {expected_file_offset}")
    # Cross-check: reading the same bytes as little-endian must NOT
    # reproduce the same offsets (proves big_endian actually selects ">").
    wrong_way = build_section_map(ddb, big_endian=False, base_address=0)
    check(dict(wrong_way) != by_name,
          "reading big-endian bytes as little-endian should disagree "
          "with the correct decoding")


def test_from_json_ok_path_separates_drb_stdout_from_drf():
    with _run_from_json() as (result, workdir):
        check(result.ok,
              f"expected ok, got stage={result.stage} stderr={result.stderr!r}")
        check(result.stage == "done", f"expected stage done, got {result.stage}")
        check(result.json_bytes == fake_drf.FIXED_JSON.encode("latin-1"),
              "json_bytes should be the untouched bytes of fake_drf's g.json")
        check(result.ddb == fake_php.FIXED_DDB,
              "ddb should match the fixed fake_php payload")
        check(result.drb_stdout == fake_php.STDOUT_MARKER,
              f"drb_stdout should be DRB's own marker alone, got {result.drb_stdout!r}")
        check(fake_drf.STDOUT_MARKER not in result.drb_stdout,
              "drb_stdout must not carry DRF's stdout")
        check(fake_drf.STDOUT_MARKER in result.stdout
              and fake_php.STDOUT_MARKER in result.stdout,
              "combined stdout should still carry both markers")
        check(result.xmb_files == {},
              f"no XMB expected in ok mode, got {sorted(result.xmb_files)}")
        check(_run_dir_count(workdir) == 0, "run dir must be removed on success")


def test_from_json_xmb_mode_collects_xmb_files():
    with _run_from_json(php_mode="xmb") as (result, workdir):
        check(result.ok,
              f"expected ok, got stage={result.stage} stderr={result.stderr!r}")
        check(result.xmb_files == {"0.XMB": fake_php.FIXED_XMB},
              f"expected 0.XMB collected, got {result.xmb_files!r}")
        check(_run_dir_count(workdir) == 0, "run dir must be removed on success")


def test_from_json_drf_failure_returns_stage_drf_and_keeps_dir():
    with _run_from_json(drf_mode="fail") as (result, workdir):
        check(not result.ok, "drf failure must not be ok")
        check(result.stage == "drf", f"expected stage drf, got {result.stage}")
        check(result.json_bytes is None, "no json_bytes on drf failure")
        check(result.ddb is None, "no ddb on drf failure")
        check(result.drb_stdout == "", "drb never ran, drb_stdout must be empty")
        check(result.xmb_files == {}, "no xmb_files on drf failure")
        check(_run_dir_count(workdir) == 1, "run dir must be kept on drf failure")


def test_from_json_drb_failure_returns_stage_drb_and_keeps_dir():
    with _run_from_json(php_mode="fail") as (result, workdir):
        check(not result.ok, "drb failure must not be ok")
        check(result.stage == "drb", f"expected stage drb, got {result.stage}")
        check(result.json_bytes is not None,
              "json_bytes from the completed drf stage should survive")
        check(result.ddb is None, "no ddb on drb failure")
        check(result.drb_stdout == "",
              "fake_php's fail mode writes no stdout before failing")
        check(result.xmb_files == {}, "no xmb_files on drb failure")
        check(_run_dir_count(workdir) == 1, "run dir must be kept on drb failure")


def test_from_json_missing_ddb_returns_stage_drb_and_keeps_dir():
    with _run_from_json(php_mode="noddb") as (result, workdir):
        check(not result.ok, "missing ddb must not be ok")
        check(result.stage == "drb", f"expected stage drb, got {result.stage}")
        check(result.ddb is None, "no ddb when drb wrote none")
        check("DDB" in result.stderr,
              f"stderr should mention the missing DDB, got {result.stderr!r}")
        check(result.xmb_files == {}, "no xmb_files when drb wrote no ddb")
        check(_run_dir_count(workdir) == 1,
              "run dir must be kept when no DDB is produced")


def test_normalise_newlines_converts_crlf_and_cr_to_lf():
    check(_normalise_newlines("a\r\nb\rc\nd") == "a\nb\nc\nd",
          "CRLF and lone CR must both become LF")


def test_normalise_newlines_empty_input_is_safe():
    check(_normalise_newlines("") == "", "empty input must stay empty")


def test_strip_banner_line_drops_exactly_one_line():
    check(_strip_banner_line("banner\nrest\nof\ntext") == "rest\nof\ntext",
          "only the first line should be dropped")


def test_strip_banner_line_single_line_leaves_nothing():
    check(_strip_banner_line("only banner") == "",
          "a banner with no following line must strip to empty")


def test_strip_banner_line_empty_input_is_safe():
    check(_strip_banner_line("") == "",
          "empty input must stay empty, not raise")


def test_normalise_clock_symbols_zeroes_only_the_real_fixture_clock_lines():
    """task-10-brief.md Step 1: the vector is DERIVED FROM THE REAL
    COMMITTED FIXTURE BYTES, not typed by hand - reads
    tests/fixtures/BLANK_EN.NEXTDAAD_EN_v3.json's own four clock symbol
    lines (YEARHIGH, YEARLOW, MONTH, DAY - drf.pas:259-262) and builds
    the expected text by substituting on those real, located bytes.
    _normalise_clock_symbols must zero exactly those four Value numbers
    and leave every other byte of the 28KB fixture untouched."""
    path = verify.FIXTURES / "BLANK_EN.NEXTDAAD_EN_v3.json"
    text = path.read_text(encoding="latin-1")

    expected = text
    matched_any = False
    for symbol in ("YEARHIGH", "YEARLOW", "MONTH", "DAY"):
        m = re.search(r'\{"symbol":"' + symbol + r'", "Value":\d+\}', text)
        check(m is not None,
              f"fixture must carry a {symbol} clock line in the exact "
              f"shape verify.py's normaliser matches")
        if m is None:
            continue
        matched_any = True
        real_line = m.group(0)
        zeroed_line = re.sub(r'"Value":\d+', '"Value":0', real_line)
        check(zeroed_line != real_line,
              f"{symbol}'s committed Value must be non-zero, or this "
              f"vector proves nothing")
        expected = expected.replace(real_line, zeroed_line)

    check(matched_any, "at least one clock line must have been found")

    normalised = verify._normalise_clock_symbols(text)
    check(normalised == expected,
          "normalisation must zero exactly the four clock lines' Value "
          "numbers, byte-precise, and touch nothing else in the file")
    check(normalised != text,
          "normalisation must actually change the real fixture text "
          "(the committed clock values are non-zero)")

    # Idempotent: normalising already-normalised text changes nothing.
    check(verify._normalise_clock_symbols(normalised) == normalised,
          "normalisation must be idempotent")

    # A symbol whose name merely CONTAINS one of the four (e.g. a
    # hypothetical "SATURDAY") must not be caught by the alternation -
    # the real fixture has no such symbol, so this pins the regex
    # itself against a synthetic line instead.
    decoy = '{"symbol":"SATURDAY", "Value":99}'
    check(verify._normalise_clock_symbols(decoy) == decoy,
          "a symbol name merely containing 'DAY' must not be normalised")


def test_verify_main_with_no_arguments_errors_with_usage_not_traceback():
    """T9 carry-over: verify.py invoked bare (no --self-check, --ndrc, or
    --from-json) must not silently fall through to an implicit self-check
    - it must argparse-error with usage text on stderr and a nonzero
    SystemExit, and it must never traceback. verify.main() takes an
    explicit argv (the "argparse guard test hook") so this is checked
    in-process rather than via subprocess."""
    import io
    from contextlib import redirect_stderr

    buf = io.StringIO()
    exit_code = "not raised"
    try:
        with redirect_stderr(buf):
            verify.main([])
    except SystemExit as e:
        exit_code = e.code

    check(exit_code != "not raised", "expected a SystemExit, got none")
    check(isinstance(exit_code, int) and exit_code != 0,
          f"expected a nonzero exit code, got {exit_code!r}")
    check("usage" in buf.getvalue().lower(),
          f"expected usage text on stderr, got {buf.getvalue()!r}")


def test_layout_for_base_override_wins_over_table():
    """task-2-brief.md Interfaces: layout_for(target, subtarget,
    base_override=None) returns (base_override, big_endian) when an
    override is given - big_endian stays the target's own value, only
    base_address is replaced."""
    base, big_endian = layout_for("ZX", "48K", base_override=0x9000)
    check(base == 0x9000, f"base_override must win over the table, got {base}")
    check(big_endian is False,
          "big_endian must still be ZX's own value, unaffected by base_override")

    base2, big_endian2 = layout_for("ST", None, base_override=0x1234)
    check(base2 == 0x1234, f"unexpected base {base2}")
    check(big_endian2 is True,
          "ST's big_endian must be unaffected by base_override")


def test_layout_for_no_override_falls_back_to_table():
    base, _ = layout_for("ZX", "48K")
    check(base == 0x8400, "omitting base_override must fall back to the table")
    base2, _ = layout_for("ZX", "48K", base_override=None)
    check(base2 == 0x8400,
          "base_override=None must fall back to the table, same as omitting it")


def test_base_override_from_flags_parses_hex_case_insensitively():
    check(verify._base_override_from_flags(("-b=0x9000",)) == 0x9000,
          "must parse -b=0x9000")
    check(verify._base_override_from_flags(("-B=0X9000",)) == 0x9000,
          "must parse -B=0X9000 case-insensitively (main.c upper-cases options)")
    check(verify._base_override_from_flags(("-x", "-b=0x100")) == 0x100,
          "must find -b= among other flags")
    check(verify._base_override_from_flags(()) is None,
          "no flags at all must give None")
    check(verify._base_override_from_flags(("-x", "-p")) is None,
          "flags with no -b= must give None")


def test_stage_run_dir_stages_dsf_and_passes_when_clean():
    tmp_root = Path(tempfile.mkdtemp(prefix="ndrc_ref_stage_clean_"))
    try:
        run_dir = tmp_root / "run"
        run_dir.mkdir()
        dsf = tmp_root / "input.dsf"
        dsf.write_text("dummy dsf source\n", encoding="utf-8")
        reference.stage_run_dir(run_dir, dsf)  # must not raise
        check((run_dir / "g.DSF").exists(), "g.DSF must be staged")
        check((run_dir / "g.DSF").read_text(encoding="utf-8")
              == "dummy dsf source\n", "g.DSF content must match the source dsf")
        check(not (run_dir / "g.tok").exists(),
              "no sidecar .tok present should mean no g.tok")
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


def test_stage_run_dir_copies_sidecar_tok_when_present():
    """Sidecar hook (task-2-brief.md Interfaces): tests/fixtures/
    <FIXTURE>.tok, when it exists beside the DSF, is copied in as g.tok
    before DRF/DRB ever run (the name DRB derives from g.json -
    drb.php:1749-1756)."""
    tmp_root = Path(tempfile.mkdtemp(prefix="ndrc_ref_stage_sidecar_"))
    try:
        run_dir = tmp_root / "run"
        run_dir.mkdir()
        dsf = tmp_root / "input.dsf"
        dsf.write_text("dummy dsf source\n", encoding="utf-8")
        sidecar = dsf.with_suffix(".tok")
        sidecar.write_bytes(b"sidecar token bytes")
        reference.stage_run_dir(run_dir, dsf)
        check((run_dir / "g.tok").exists(),
              "sidecar .tok must be copied to g.tok")
        check((run_dir / "g.tok").read_bytes() == b"sidecar token bytes",
              "g.tok content must match the sidecar file")
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


def test_stage_run_dir_raises_when_stale_xmb_already_present():
    """Stale-XMB guard (task-2-brief.md Interfaces): a freshly staged run
    dir must carry no *.XMB - always true today since run dirs are
    tempfile.mkdtemp-fresh, but the assert pins the guarantee (spec
    section 2) as a hard failure."""
    tmp_root = Path(tempfile.mkdtemp(prefix="ndrc_ref_stage_stale_"))
    try:
        run_dir = tmp_root / "run"
        run_dir.mkdir()
        (run_dir / "leftover.XMB").write_bytes(b"stale")
        dsf = tmp_root / "input.dsf"
        dsf.write_text("dummy\n", encoding="utf-8")
        raised = False
        try:
            reference.stage_run_dir(run_dir, dsf)
        except AssertionError as e:
            raised = True
            check("leftover.XMB" in str(e),
                  f"assertion message should name the stale file, got {e!r}")
        check(raised,
              "stage_run_dir must raise when a stale *.XMB is already present")
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


def _make_run_dir_cfg(prefix: str):
    tmp_root = Path(tempfile.mkdtemp(prefix=prefix))
    cfg = _make_cfg(tmp_root)
    dsf = tmp_root / "input.dsf"
    dsf.write_text("dummy dsf source\n", encoding="utf-8")
    return tmp_root, cfg, dsf


def _spy_on_run(calls: list) -> None:
    """Monkeypatches reference._run to record every args list it is
    called with (drf, then drb), while still delegating to the real
    implementation so the fakes actually run."""
    original = reference._run

    def spy(args, cwd):
        calls.append(list(args))
        return original(args, cwd)

    reference._run = spy
    return original


def test_run_reference_appends_combo_flags_to_drb_invocation():
    calls: list = []
    original_run = _spy_on_run(calls)
    tmp_root, cfg, dsf = _make_run_dir_cfg("ndrc_ref_flags_")
    try:
        combo = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
                     classic=False, flags=("-x", "-b=0x9000"))
        result = reference.run_reference(cfg, dsf, combo)
        check(result.ok, f"expected ok, got stage={result.stage}")
        drb_calls = [c for c in calls if str(cfg.drb) in c]
        check(len(drb_calls) == 1, f"expected one DRB call, got {len(drb_calls)}")
        check(drb_calls[0][-2:] == ["-x", "-b=0x9000"],
              f"combo.flags must be appended verbatim, got {drb_calls[0]}")
    finally:
        reference._run = original_run
        shutil.rmtree(tmp_root, ignore_errors=True)


def test_run_reference_from_json_appends_combo_flags_to_drb_invocation():
    calls: list = []
    original_run = _spy_on_run(calls)
    tmp_root, cfg, dsf = _make_run_dir_cfg("ndrc_ref_fromjson_flags_")
    try:
        combo = Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True,
                     classic=False, flags=("-p",))
        result = reference.run_reference_from_json(cfg, dsf, combo)
        check(result.ok, f"expected ok, got stage={result.stage}")
        drb_calls = [c for c in calls if str(cfg.drb) in c]
        check(len(drb_calls) == 1, f"expected one DRB call, got {len(drb_calls)}")
        check(drb_calls[0][-1] == "-p",
              f"combo.flags must be appended verbatim, got {drb_calls[0]}")
    finally:
        reference._run = original_run
        shutil.rmtree(tmp_root, ignore_errors=True)


def test_stage_ndrc_run_dir_copies_sidecar_and_writes_json():
    """verify.py's own ndrc run-dir staging (task-2-brief.md Interfaces:
    "wherever ndrc's run dir is staged in verify.py") mirrors
    reference.py's stage_run_dir - sidecar .tok copy plus the same
    stale-XMB guard."""
    tmp_fixtures = Path(tempfile.mkdtemp(prefix="ndrc_verify_fixtures_"))
    tmp_run = Path(tempfile.mkdtemp(prefix="ndrc_verify_rundir_"))
    old_fixtures = verify.FIXTURES
    try:
        (tmp_fixtures / "MYFIX.tok").write_bytes(b"sidecar bytes")
        verify.FIXTURES = tmp_fixtures
        verify._stage_ndrc_run_dir(tmp_run, b'{"json": true}', "MYFIX")
        check((tmp_run / "g.json").read_bytes() == b'{"json": true}',
              "g.json must hold the given bytes")
        check((tmp_run / "g.tok").exists(), "sidecar must be copied to g.tok")
        check((tmp_run / "g.tok").read_bytes() == b"sidecar bytes",
              "g.tok content must match the sidecar file")
    finally:
        verify.FIXTURES = old_fixtures
        shutil.rmtree(tmp_fixtures, ignore_errors=True)
        shutil.rmtree(tmp_run, ignore_errors=True)


def test_stage_ndrc_run_dir_no_sidecar_means_no_g_tok():
    tmp_fixtures = Path(tempfile.mkdtemp(prefix="ndrc_verify_fixtures2_"))
    tmp_run = Path(tempfile.mkdtemp(prefix="ndrc_verify_rundir2_"))
    old_fixtures = verify.FIXTURES
    try:
        verify.FIXTURES = tmp_fixtures
        verify._stage_ndrc_run_dir(tmp_run, b"{}", "NOFIX")
        check(not (tmp_run / "g.tok").exists(), "no sidecar means no g.tok")
    finally:
        verify.FIXTURES = old_fixtures
        shutil.rmtree(tmp_fixtures, ignore_errors=True)
        shutil.rmtree(tmp_run, ignore_errors=True)


def test_stage_ndrc_run_dir_raises_on_stale_xmb():
    tmp_run = Path(tempfile.mkdtemp(prefix="ndrc_verify_stalexmb_"))
    try:
        (tmp_run / "leftover.XMB").write_bytes(b"stale")
        raised = False
        try:
            verify._stage_ndrc_run_dir(tmp_run, b"{}", "NOFIX")
        except AssertionError:
            raised = True
        check(raised, "_stage_ndrc_run_dir must raise on a pre-existing *.XMB")
    finally:
        shutil.rmtree(tmp_run, ignore_errors=True)


def test_multi_file_manifest_entry_shapes():
    """task-2-brief.md Step 1: multi-file manifest round-trip. An entry
    with no xmb_files keeps the existing single-DDB shape; one with
    xmb_files takes the "files" wrapper, "ddb" plus each XMB name."""
    single = gen_goldens.manifest_entry(b"abc", {})
    check(set(single) == {"sha256", "bytes"},
          f"no-xmb entry must keep the old shape, got {sorted(single)}")
    check(single["bytes"] == 3, "bytes must be the ddb length")
    check(single["sha256"] == hashlib.sha256(b"abc").hexdigest(),
          "sha256 must be the ddb's own digest")
    check(gen_goldens.entry_files(single) == {"ddb": single},
          "entry_files must fold a single-DDB entry under the name 'ddb'")

    multi = gen_goldens.manifest_entry(b"ddb-bytes", {"0.XMB": b"xmb-bytes"})
    check(set(multi) == {"files"},
          f"an xmb-carrying entry must use the files wrapper, got {sorted(multi)}")
    check(set(multi["files"]) == {"ddb", "0.XMB"},
          f"unexpected file names {sorted(multi['files'])}")
    check(multi["files"]["ddb"]["bytes"] == len(b"ddb-bytes"),
          "ddb file meta must carry the ddb's own byte count")
    check(multi["files"]["0.XMB"]["bytes"] == len(b"xmb-bytes"),
          "0.XMB file meta must carry the xmb's own byte count")
    check(gen_goldens.entry_files(multi) == multi["files"],
          "entry_files on a multi-file entry must return its own files mapping")


def test_multi_file_golden_write_and_round_trip():
    """task-2-brief.md Interfaces: multi-file goldens store the XMB bytes
    beside the DDB as tests/goldens/<FIXTURE>/<slug>.<xmbname>."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        ddb = b"ddb-bytes-1234"
        xmb_files = {"0.XMB": b"xmb-bytes-5678"}
        entry = gen_goldens.manifest_entry(ddb, xmb_files)
        gen_goldens.write_golden_files(
            "BLANK_EN", "NEXTDAAD_EN_v3_opt_x", ddb, xmb_files, root=root)

        ddb_path = gen_goldens.golden_file_path(
            "BLANK_EN", "NEXTDAAD_EN_v3_opt_x", "ddb", root=root)
        check(ddb_path == root / "BLANK_EN" / "NEXTDAAD_EN_v3_opt_x.ddb",
              f"ddb naming must be <slug>.ddb, got {ddb_path}")
        xmb_path = gen_goldens.golden_file_path(
            "BLANK_EN", "NEXTDAAD_EN_v3_opt_x", "0.XMB", root=root)
        check(xmb_path == root / "BLANK_EN" / "NEXTDAAD_EN_v3_opt_x.0.XMB",
              f"xmb naming must be <slug>.<xmbname>, got {xmb_path}")

        for name, meta in gen_goldens.entry_files(entry).items():
            path = gen_goldens.golden_file_path(
                "BLANK_EN", "NEXTDAAD_EN_v3_opt_x", name, root=root)
            check(path.exists(), f"{name}: golden file must exist on disk")
            data = path.read_bytes()
            check(len(data) == meta["bytes"], f"{name}: byte count mismatch")
            check(hashlib.sha256(data).hexdigest() == meta["sha256"],
                  f"{name}: sha256 mismatch")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}")
        print(f"reference: {len(FAILURES)} failures")
        return 1
    print("reference: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
