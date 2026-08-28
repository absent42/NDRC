# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""CLI argument-parsing tests for ndrc --from-json. Run directly:
python test_cli.py

Deliberately not a pytest suite - see test_matrix.py's docstring for why
(the check()/FAILURES pattern needs main() to report correctly).

No PHP reference needed here: every expected string is transcribed
verbatim from task-2-brief.md's case list, itself measured against
drb.php:1358-1397/1707-1780/1233-1247 (Rule 0.1). ndrc's own first
stdout line (the version banner) is stripped before every comparison,
since its exact text is not part of DRB's ported behaviour.

The ndrc binary is located via the NDRC environment variable, defaulting
to ../../ndrc (relative to this script's own directory, not the cwd) -
appending .exe if the bare path does not exist but the .exe does
(Windows).
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURE = HERE.parent / "fixtures" / "BLANK_EN.NEXTDAAD_EN_v3.json"
DSF_FIXTURE = HERE.parent / "fixtures" / "BLANK_EN.DSF"
IFDEFS_DSF_FIXTURE = HERE.parent / "fixtures" / "IFDEFS.DSF"

FAILURES = []


def check(cond, label):
    if not cond:
        FAILURES.append(label)


def _resolve_ndrc() -> Path:
    raw = os.environ.get("NDRC", "../../ndrc")
    p = Path(raw)
    if not p.is_absolute():
        p = (HERE / p).resolve()
    if not p.exists():
        exe = Path(str(p) + ".exe")
        if exe.exists():
            p = exe
    return p


NDRC = _resolve_ndrc()


def run(args, cwd=None):
    """Runs ndrc, returning (returncode, stdout-after-banner-line)."""
    proc = subprocess.run(
        [str(NDRC)] + args, capture_output=True, text=True, cwd=cwd
    )
    lines = proc.stdout.splitlines(keepends=True)
    rest = "".join(lines[1:]) if lines else ""
    return proc.returncode, rest


def _fixture_copy(tmpdir: Path, name: str = "g.json") -> Path:
    dest = tmpdir / name
    shutil.copyfile(FIXTURE, dest)
    return dest


def _dsf_copy(tmpdir: Path, name: str = "g.dsf") -> Path:
    dest = tmpdir / name
    shutil.copyfile(DSF_FIXTURE, dest)
    return dest


# --- Case 1: invalid target machine -------------------------------------

def test_case01_target_invalid():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(
            ["--from-json", "SPECTRUM", "EN", str(tmpdir / "x.json")]
        )
        check(rc == 2, f"case1: expected exit 2, got {rc}")
        check(
            out == "Error: Invalid target machine 'SPECTRUM'.\n",
            f"case1: unexpected stdout {out!r}",
        )


# --- Case 2: invalid subtarget --------------------------------------------

def test_case02_subtarget_invalid():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(
            ["--from-json", "ZX", "32K", "EN", str(tmpdir / "x.json")]
        )
        check(rc == 2, f"case2: expected exit 2, got {rc}")
        expected = (
            "Debug: Checking subtarget 32K for target ZX\n"
            "Error: Invalid subtarget '32K' for target 'ZX'.\n"
        )
        check(out == expected, f"case2: unexpected stdout {out!r}")


# --- Case 3: subtarget echo + Target line, then file-not-found -----------

def test_case03_subtarget_echo_and_target_line():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(
            ["--from-json", "ZX", "48K", "EN", str(tmpdir / "missing.json")]
        )
        check(rc == 2, f"case3: expected exit 2, got {rc}")
        expected = (
            "Debug: Checking subtarget 48K for target ZX\n"
            "Target: ZX (48K)\n"
            "Error: File not found.\n"
        )
        check(out == expected, f"case3: unexpected stdout {out!r}")


# --- Case 4: bare target, no subtarget echo -------------------------------

def test_case04_bare_target_no_echo():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(
            ["--from-json", "CPC", "EN", str(tmpdir / "missing.json")]
        )
        check(rc == 2, f"case4: expected exit 2, got {rc}")
        expected = "Target: CPC\nError: File not found.\n"
        check(out == expected, f"case4: unexpected stdout {out!r}")


# --- Case 5: invalid language ---------------------------------------------

def test_case05_language_invalid():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(
            ["--from-json", "CPC", "XX", str(tmpdir / "x.json")]
        )
        check(rc == 2, f"case5: expected exit 2, got {rc}")
        expected = "Target: CPC\nError: Invalid target language.\n"
        check(out == expected, f"case5: unexpected stdout {out!r}")


# --- Case 6: unknown option, upper-cased before the error ----------------

def test_case06_unknown_option():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir, "in.json")
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", str(inp), "out.ddb", "-q"]
        )
        check(rc == 2, f"case6: expected exit 2, got {rc}")
        check(
            out.endswith("Error: -Q is not a valid option.\n"),
            f"case6: unexpected stdout {out!r}",
        )


# --- Case 7: two output names, positionals not upper-cased ---------------

def test_case07_two_output_names():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir, "in.json")
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", str(inp), "a.ddb", "b.ddb"]
        )
        check(rc == 2, f"case7: expected exit 2, got {rc}")
        check(
            out.endswith("Error: Bad parameter: b.ddb.\n"),
            f"case7: unexpected stdout {out!r}",
        )


# --- Case 8: -d/-p/-np/-b= all wired for real (task-6-brief.md) ------------

def test_case08_base_address_bounds():
    """-b='s 1..0xFFFF bounds check (drb.php:1378-1384): the error text
    echoes the FULL upper-cased "-B=..." argument, not just its value
    half."""
    cases = [
        ("-b=0", "-B=0"),
        ("-b=0x10000", "-B=0X10000"),
    ]
    for raw_flag, expected_arg in cases:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            inp = _fixture_copy(tmpdir, "in.json")
            rc, out = run(
                ["--from-json", "NEXTDAAD", "EN", str(inp), raw_flag]
            )
            check(rc == 2, f"case8 {raw_flag}: expected exit 2, got {rc}")
            expected_tail = f"Error: Invalid base address in {expected_arg}.\n"
            check(
                out.endswith(expected_tail),
                f"case8 {raw_flag}: unexpected stdout {out!r}",
            )


def test_case08b_np_p_conflict():
    """-np and -p together hit the existing forced-padding conflict
    (drb.php:1773), already in the CLI ahead of this task."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir, "in.json")
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", str(inp), "-np", "-p"]
        )
        check(rc == 2, f"case8b: expected exit 2, got {rc}")
        expected_tail = (
            "Error: You can't force padding and no padding at the same "
            "time.\n"
        )
        check(out.endswith(expected_tail), f"case8b: unexpected stdout {out!r}")


def test_case08c_debug_padding_base_accepted():
    """-d/-p/-np/-b= are no longer refused (the Phase 1c boundary this
    case used to pin is gone - all four are wired for real now). cwd is
    the fixture's own tmpdir (matching case11/13/14's own pattern) so a
    successful run's output.ddb does not leak outside the temp dir."""
    for flag in ("-d", "-p", "-np", "-b=0x100"):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            _fixture_copy(tmpdir, "g.json")
            rc, out = run(
                ["--from-json", "NEXTDAAD", "EN", "g.json", "out.ddb", flag],
                cwd=str(tmpdir),
            )
            check(rc == 0, f"case8c {flag}: expected exit 0, got {rc}\n{out}")


# --- Case 9: -ch on a non-C64/CP4 target ----------------------------------

def test_case09_ch_wrong_target():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", str(inp), "out.ddb", "-ch"]
        )
        check(rc == 2, f"case9: expected exit 2, got {rc}")
        expected_tail = (
            "Error: Adding C64 header was requested but target is not "
            "C64 or CP4.\n"
        )
        check(out.endswith(expected_tail), f"case9: unexpected stdout {out!r}")


# --- Case 10: -3h on a non-ZX target ---------------------------------------

def test_case10_3h_wrong_target():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", str(inp), "out.ddb", "-3h"]
        )
        check(rc == 2, f"case10: expected exit 2, got {rc}")
        expected_tail = (
            "Error: Adding +3DOS header was requested but target is "
            "not ZX Spectrum.\n"
        )
        check(out.endswith(expected_tail), f"case10: unexpected stdout {out!r}")


# --- Case 11: default output name -----------------------------------------

def test_case11_default_output_name():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir)
        rc, out = run(["--from-json", "NEXTDAAD", "EN", "g.json"], cwd=str(tmpdir))
        check(rc == 0, f"case11: expected exit 0, got {rc}\n{out}")
        check(
            (tmpdir / "g.DDB").exists(),
            "case11: expected g.DDB (upper-case extension) to be created",
        )

    # Re-run with -v: DRB's replace_extension carries pathinfo()'s
    # dirname prefix even for a bare filename (dirname is "." there,
    # never empty/falsy) - confirmed against a live php.exe. The
    # created-file summary line must show that platform-native prefix
    # (drb.php:280-286, DIRECTORY_SEPARATOR), computed here via os.sep
    # so the assertion holds on both the Windows and POSIX CI legs.
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", "g.json", "-v"], cwd=str(tmpdir)
        )
        check(rc == 0, f"case11 -v: expected exit 0, got {rc}\n{out}")
        expected_line = f".{os.sep}g.DDB for NEXTDAAD created.\n"
        check(
            expected_line in out,
            f"case11 -v: expected {expected_line!r} in stdout, got {out!r}",
        )


# --- Case 12: input == output ----------------------------------------------

def test_case12_input_equals_output():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", "g.json", "g.json"],
            cwd=str(tmpdir),
        )
        check(rc == 2, f"case12: expected exit 2, got {rc}")
        expected_tail = (
            "Error: Input and output file name cannot be the same.\n"
        )
        check(out.endswith(expected_tail), f"case12: unexpected stdout {out!r}")


def test_case12b_input_equals_output_before_padding_conflict():
    """Ordering (drb.php:1770-1773; final-review F1): the same-name check
    runs BEFORE the -p/-np padding-conflict check, so a run that hits both
    (same input/output name, -p and -np both given) must report the
    same-name Error, not the padding one."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", "g.json", "g.json", "-p", "-np"],
            cwd=str(tmpdir),
        )
        check(rc == 2, f"case12b: expected exit 2, got {rc}")
        expected_tail = (
            "Error: Input and output file name cannot be the same.\n"
        )
        check(out.endswith(expected_tail), f"case12b: unexpected stdout {out!r}")


# --- Case 13: case-insensitive target/language, -V matches -v ------------

def test_case13_case_insensitive():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        inp = _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "nextdaad", "en", "g.json", "out.ddb", "-V"],
            cwd=str(tmpdir),
        )
        check(rc == 0, f"case13: expected exit 0, got {rc}\n{out}")
        check(
            "Target: NEXTDAAD" in out,
            f"case13: expected 'Target: NEXTDAAD' in stdout, got {out!r}",
        )
        check(
            "Verbose mode on\n" in out,
            f"case13: -V should have set verbose mode, got {out!r}",
        )
        check(
            (tmpdir / "out.ddb").exists(),
            "case13: expected out.ddb to be created",
        )


# --- Case 14: -x accepted, dumps TX sections to 0.XMB ---------------------

def test_case14_x_dumps_to_xmb():
    """task-4-brief.md Step 2's -x acceptance case: a happy run on the
    committed NEXTDAAD fixture with -x exits 0 and leaves a 0.XMB in the
    run's own cwd. BLANK_EN.NEXTDAAD_EN_v3.json carries 1 user message
    and 3 locations (none of them default sysmess, so none are exempted
    by drb.php:532/LAST_DEFAULT_SYSMESS) - all four are non-stx messages,
    so -x's dump arm always sends at least those to 0.XMB, making a
    nonzero-size file the correct expectation here, not just the easy
    one."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _fixture_copy(tmpdir)
        rc, out = run(
            ["--from-json", "NEXTDAAD", "EN", "g.json", "out.ddb", "-x"],
            cwd=str(tmpdir),
        )
        check(rc == 0, f"case14: expected exit 0, got {rc}\n{out}")
        xmb = tmpdir / "0.XMB"
        check(xmb.exists(), "case14: expected 0.XMB to be created")
        if xmb.exists():
            check(
                xmb.stat().st_size > 0,
                f"case14: expected nonzero-size 0.XMB, got {xmb.stat().st_size} bytes",
            )


# --- Ordering: missing input reports File not found before a bad option --

def test_order_missing_input_before_bad_option():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(
            [
                "--from-json",
                "NEXTDAAD",
                "EN",
                str(tmpdir / "missing.json"),
                "-q",
            ]
        )
        check(rc == 2, f"order: expected exit 2, got {rc}")
        expected = "Target: NEXTDAAD\nError: File not found.\n"
        check(out == expected, f"order: unexpected stdout {out!r}")


# --------------------------------------------------------------------
# --to-json: the DRF front-end CLI (drf.pas WHOLE, task-9-brief.md).
# Cases measured against live drf.exe (D:/DRC/src, branch nextdaad,
# verified) 2026-08-27, using tests/fixtures/BLANK_EN.DSF as the
# reference probe file (renamed g.dsf per-tempdir, matching every
# --from-json case's own g.json convention above). Every expected
# string below is a transcription of that live run, not an inference.
# --------------------------------------------------------------------

def test_to_json_unknown_option():
    """PORT: drf.pas:410 ParamError('Invalid option: ' + AuxString) -
    the bare `<msg>.` shape (diag_param_error), NOT --from-json's
    "Error: " prefix. Live-pinned: `drf.exe NEXTDAAD probe.dsf out.json
    -bogus` -> `Invalid option: -bogus.`, exit 2."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        rc, out = run(
            ["--to-json", "NEXTDAAD", "g.dsf", "out.json", "-bogus"],
            cwd=str(tmpdir),
        )
        check(rc == 2, f"to_json unknown option: expected exit 2, got {rc}")
        check(
            out == "Invalid option: -bogus.\n",
            f"to_json unknown option: unexpected stdout {out!r}",
        )


def test_to_json_missing_file():
    """PORT: drf.pas:338 ParamError('Input file not found: "' +
    InputFileName + '"'). Live-pinned: `drf.exe NEXTDAAD nosuchfile.dsf`
    -> `Input file not found: "nosuchfile.dsf".`, exit 2."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(["--to-json", "NEXTDAAD", "nosuchfile.dsf"], cwd=str(tmpdir))
        check(rc == 2, f"to_json missing file: expected exit 2, got {rc}")
        check(
            out == 'Input file not found: "nosuchfile.dsf".\n',
            f"to_json missing file: unexpected stdout {out!r}",
        )


def test_to_json_invalid_subtarget():
    """PORT: drf.pas:334 ParamError - a subtarget-requiring target
    (ZX/PC/MSX2/ZX81) with an unrecognised subtarget. Fix round 1: the
    Pascal literal itself already ends in a period, and ParamError
    appends another, so the reference DOUBLES it - re-captured as raw
    bytes this round (`drf.exe ZX 32K probe.dsf`):
    `"32K" is not a valid subtarget for target "ZX". Please specify a
    valid subtarget. Call DRF without parameters for more
    information..`, CRLF, exit 2 (the original single-period pin here
    was a transcription error - 19.40's doubled-period mechanism, which
    already applied to SyntaxError's deprecation texts, extends to
    ParamError too)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        rc, out = run(["--to-json", "ZX", "32K", "g.dsf"], cwd=str(tmpdir))
        check(rc == 2, f"to_json invalid subtarget: expected exit 2, got {rc}")
        expected = (
            '"32K" is not a valid subtarget for target "ZX". Please '
            "specify a valid subtarget. Call DRF without parameters for "
            "more information..\n"
        )
        check(out == expected, f"to_json invalid subtarget: unexpected stdout {out!r}")


def test_to_json_missing_end_section():
    """PORT: drf.pas:419 ParamError via CheckEND - scans the RAW input
    file (before Preparse) for a line equal to exactly "/END". Fix
    round 1: drf.pas:419's own literal already ends in a period;
    ParamError's appended period doubles it - re-captured as raw bytes
    this round: `...if any..`, CRLF (19.40's mechanism, extended to
    ParamError - the original single-period pin here was a
    transcription error)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        (tmpdir / "noend.dsf").write_text("/CTL\n_\n", encoding="ascii")
        rc, out = run(["--to-json", "NEXTDAAD", "noend.dsf"], cwd=str(tmpdir))
        check(rc == 2, f"to_json missing /END: expected exit 2, got {rc}")
        expected = (
            "Input file has no /END section. Please make sure /END "
            "it's in main file, not in #include files, if any..\n"
        )
        check(out == expected, f"to_json missing /END: unexpected stdout {out!r}")


def test_to_json_semantic_option_conflict():
    """PORT: drf.pas:415 ParamError - -no-semantic and -semantic-warnings
    together. Fix round 1: drf.pas:415's own literal already ends in a
    period; ParamError's appended period doubles it - re-captured as
    raw bytes this round: `...warnings..`, CRLF (19.40's mechanism,
    extended to ParamError - the original single-period pin here was a
    transcription error)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        rc, out = run(
            [
                "--to-json", "NEXTDAAD", "g.dsf", "out.json",
                "-no-semantic", "-semantic-warnings",
            ],
            cwd=str(tmpdir),
        )
        check(rc == 2, f"to_json semantic conflict: expected exit 2, got {rc}")
        expected = (
            "You can't avoid semantic checking and at the same time "
            "expect semantic warnings..\n"
        )
        check(out == expected, f"to_json semantic conflict: unexpected stdout {out!r}")


def test_to_json_message_option_conflict():
    """PORT: drf.pas:416 ParamError - -force-normal-messages and
    -force-x-messages together ("XMesages" - missing an 's' - is the
    reference's own typo, kept verbatim). Fix round 1: drf.pas:416's
    own literal already ends in a period; ParamError's appended period
    doubles it - live-pinned as raw bytes this round (`drf.exe NEXTDAAD
    probe.dsf out.json -force-normal-messages -force-x-messages`):
    `You can't force XMesages and normal messages at the same time..`,
    CRLF, exit 2. This is the fourth ParamError doubled-period site;
    the previous round left it unpinned."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        rc, out = run(
            [
                "--to-json", "NEXTDAAD", "g.dsf", "out.json",
                "-force-normal-messages", "-force-x-messages",
            ],
            cwd=str(tmpdir),
        )
        check(rc == 2, f"to_json message conflict: expected exit 2, got {rc}")
        expected = (
            "You can't force XMesages and normal messages at the same "
            "time..\n"
        )
        check(out == expected, f"to_json message conflict: unexpected stdout {out!r}")


def test_to_json_too_few_args_exit_class():
    """NDRC's own bounds check stands in for drf.pas's 14-paragraph
    SYNTAX() text (ParamCount()<2, drf.pas:327) but keeps SYNTAX()'s
    OWN exit class (1, analysis 13.1) rather than ParamError's (2) -
    see main.c's file header PORT NOTE."""
    rc, out = run(["--to-json", "NEXTDAAD"])
    check(rc == 1, f"to_json too few args: expected exit 1, got {rc}\n{out}")
    check(
        out.startswith("usage:"),
        f"to_json too few args: expected NDRC's own usage line, got {out!r}",
    )


# --- 19.4: output-name detection is heuristic (a dot anywhere in the
# trailing argument makes it the output name; otherwise it is the
# symbol list and the slot is NOT consumed) -------------------------

def test_to_json_dot_heuristic_dotless_is_symbol():
    """Live-pinned: `drf.exe NEXTDAAD probe.dsf mysymbol -verbose` adds
    symbol MYSYMBOL=1 and writes the DEFAULT `probe.json` (mysymbol
    itself is never consumed as an output name)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        rc, out = run(
            ["--to-json", "NEXTDAAD", "g.dsf", "mysymbol", "-verbose"],
            cwd=str(tmpdir),
        )
        check(rc == 0, f"to_json dotless heuristic: expected exit 0, got {rc}\n{out}")
        check(
            "Added Symbol: MYSYMBOL=1" in out,
            f"to_json dotless heuristic: expected symbol injection, got {out!r}",
        )
        check(
            (tmpdir / "g.json").exists(),
            "to_json dotless heuristic: expected default g.json output name",
        )


def test_to_json_dot_heuristic_dotted_is_output():
    """Live-pinned: `drf.exe NEXTDAAD probe.dsf my.out -verbose` uses
    `my.out` verbatim as the output path (no extension massaging) and
    writes no default `<input>.json`."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        rc, out = run(
            ["--to-json", "NEXTDAAD", "g.dsf", "my.out"], cwd=str(tmpdir)
        )
        check(rc == 0, f"to_json dotted heuristic: expected exit 0, got {rc}\n{out}")
        check(
            (tmpdir / "my.out").exists(),
            "to_json dotted heuristic: expected my.out to be used as the output path",
        )
        check(
            not (tmpdir / "g.json").exists(),
            "to_json dotted heuristic: default g.json must not be created",
        )


def test_to_json_happy_run_stage_lines():
    """PORT: analysis 14.3's stage-by-stage stdout. Live-pinned (post-
    banner-strip on both sides): `drf.exe NEXTDAAD probe.dsf` ->
    'Reading probe.dsf\\nChecking Syntax...\\nUpdating forward
    references...\\nGenerating <out> [Classic mode OFF]\\n<out>
    generated.\\n', exit 0."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        rc, out = run(["--to-json", "NEXTDAAD", "g.dsf"], cwd=str(tmpdir))
        check(rc == 0, f"to_json happy run: expected exit 0, got {rc}\n{out}")
        expected = (
            "Reading g.dsf\n"
            "Checking Syntax...\n"
            "Updating forward references...\n"
            "Generating g.json [Classic mode OFF]\n"
            "g.json generated.\n"
        )
        check(out == expected, f"to_json happy run: unexpected stdout {out!r}")
        check((tmpdir / "g.json").exists(), "to_json happy run: expected g.json to be created")
        check(
            not (tmpdir / "g.___").exists(),
            "to_json happy run: the .___ temp file must be deleted on success",
        )


def test_to_json_accented_additional_symbol_folds():
    """Context (task-9-brief.md): argv positional SYMBOLS go through
    symbols_add and thus the FULL cp1252 fold (str.h's
    str_upper_latin1) - this proves the fold applies on the CLI path,
    not just the lexer's #define path. Captured as RAW BYTES (not
    text=True): the folded byte (0xC9, "E-acute") is not valid UTF-8 on
    its own, and this platform's text-mode pipe decoding silently
    replaces invalid bytes with U+FFFD, which would corrupt the exact
    byte under test. 0xE9 ('e-acute', cp1252) -> 0xC9 ('E-acute') is
    str.c's own pinned latin1_upper flat -0x20 shift, live-verified for
    exactly this CLI path in Task 4's fix round and re-confirmed against
    ndrc.exe directly this session (both a native CreateProcess
    invocation and this exact subprocess.run mechanism agree)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _dsf_copy(tmpdir)
        sym = "caf" + chr(0xE9)  # "café" - lowercase e-acute, U+00E9
        proc = subprocess.run(
            [str(NDRC), "--to-json", "NEXTDAAD", "g.dsf", "out.json", "-verbose", sym],
            capture_output=True,
            cwd=str(tmpdir),
        )
        check(
            proc.returncode == 0,
            f"to_json accented symbol: expected exit 0, got {proc.returncode}",
        )
        check(
            b"Added Symbol: CAF\xc9=1" in proc.stdout,
            "to_json accented symbol: expected folded b'CAF\\xc9=1' in "
            f"stdout, got {proc.stdout!r}",
        )


# --------------------------------------------------------------------
# Quoted-expression operands: GetExpressionValue's call-site contract
# (USintactic.pas:114-139, analysis 29.2). Every expected text below is
# a transcription of a live drf.exe run on 2026-08-27 using exactly the
# DSF shape _expr_dsf() builds here - the analysis 29.9 probe battery's
# own environment (a BLANK_EN.DSF copy plus a `/PRO 7` holding one
# `SET "<operand>"`, or a leading `#define BAD "<operand>"`), compiled
# `NEXTDAAD g.dsf out.json -v3`.
#
# The location prefix (`<line>:<col>:g.dsf: `) is left out of the
# transcribed literals and checked by shape instead, so an edit to the
# fixture cannot silently invalidate a text; the two shell-composition
# cases pin the prefix too, byte for byte, against a live reference run
# when one is configured (see _drf_path).
# --------------------------------------------------------------------

EXPR_LINE_RE = re.compile(r"^\d+:\d+:g\.dsf: (.*)\n\Z")


def _drf_path():
    """The reference DRF binary from oracle.local.json, or None.

    The file is gitignored and machine-specific (the CI oracle job has
    it, a bare clone does not), so every case that uses it must still
    assert something on its own without it."""
    cfg = HERE / "oracle.local.json"
    if not cfg.exists():
        return None
    try:
        raw = json.loads(cfg.read_text(encoding="utf-8"))
    except ValueError:
        return None
    drf = Path(raw.get("drf", ""))
    return drf if drf.exists() else None


def _expr_dsf(tmpdir: Path, operand: str, mode: str = "condact") -> Path:
    """BLANK_EN.DSF + one quoted-expression operand, as g.dsf.

    mode "condact" appends a `/PRO 7` whose single entry is
    `SET "<operand>"` (SET: opcode 47, one flagno parameter, no
    semantic surprise); mode "define" prepends
    `#define BAD "<operand>"`. Written as latin-1: the fixture carries
    a cp1252 byte in a comment and is not UTF-8."""
    src = DSF_FIXTURE.read_bytes().decode("latin-1")
    if mode == "condact":
        body = '\n/PRO 7\n> _       _     SET     "%s"\n\n' % operand
        src = src.replace("/END", body + "/END")
    else:
        src = '#define BAD "%s"\n' % operand + src
    dest = tmpdir / "g.dsf"
    dest.write_bytes(src.encode("latin-1"))
    return dest


def _expr_run(tmpdir: Path, extra=()):
    """ndrc --to-json on the generated g.dsf; (rc, stdout-after-banner).

    `extra` appends trailing arguments - an option or a symbol list -
    which drf.pas:351-413 consumes in one order-free loop after the
    output name; _ref_run takes the same argument so both sides always
    get the identical tail."""
    return run(
        ["--to-json", "NEXTDAAD", "g.dsf", "out.json", "-v3", *extra],
        cwd=str(tmpdir),
    )


def _ref_run(tmpdir: Path, extra=()):
    """The same compile through the reference; (rc, stdout-after-banner)
    with the banner line stripped exactly as run() does."""
    proc = subprocess.run(
        [str(_drf_path()), "NEXTDAAD", "g.dsf", "out.json", "-v3", *extra],
        capture_output=True, cwd=str(tmpdir),
    )
    # Decoded from raw bytes (the DSF and its diagnostics are cp1252,
    # not UTF-8), so the CRLF the Pascal runtime writes has to be
    # normalised by hand - run()'s text=True does it for ndrc.
    text = proc.stdout.decode("latin-1").replace("\r\n", "\n")
    lines = text.splitlines(keepends=True)
    return proc.returncode, "".join(lines[1:])


def _last_line(out: str) -> str:
    lines = out.splitlines(keepends=True)
    return lines[-1] if lines else ""


def _expr_error(label, operand, expected, mode="condact", rc_want=1):
    """Runs one error-shaped operand and pins the final stdout line's
    text (after the location prefix) plus the exit code."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _expr_dsf(tmpdir, operand, mode)
        rc, out = _expr_run(tmpdir)
        check(rc == rc_want, f"{label}: expected exit {rc_want}, got {rc}\n{out}")
        m = EXPR_LINE_RE.match(_last_line(out))
        check(m is not None, f"{label}: no `<line>:<col>:g.dsf: ...` line in {out!r}")
        if m is not None:
            check(m.group(1) == expected,
                  f"{label}: expected {expected!r}, got {m.group(1)!r}")


def _find_process(doc, number):
    for proc in doc["processes"]:
        if proc.get("Value") == number:
            return proc
    return None


# --- P23: ShortString truncation, then the quote strip (defect 19.32) ---

def test_expr_p23_shortstring_truncation_is_a_wrong_value():
    """A 256-byte operand (250 spaces + `1+29` between quotes) loses its
    closing quote to `AuxStr : ShortString := CurrentText`
    (USintactic.pas:124), so Copy(2, len-2) at USintactic.pas:125 eats
    the final '9' rather than a quote. Live-pinned: exit 0, process 7's
    SET compiles `"Param1":3` - not 30, and not an error."""
    operand = " " * 250 + "1+29"
    assert len(operand) + 2 == 256
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _expr_dsf(tmpdir, operand)
        rc, out = _expr_run(tmpdir)
        check(rc == 0, f"P23: expected exit 0, got {rc}\n{out}")
        doc_path = tmpdir / "out.json"
        check(doc_path.exists(), "P23: expected out.json to be created")
        if doc_path.exists():
            doc = json.loads(doc_path.read_text(encoding="latin-1"))
            proc = _find_process(doc, 7)
            check(proc is not None, "P23: no process 7 in the JSON")
            if proc is not None:
                param1 = proc["entries"][0]["condacts"][0]["Param1"]
                check(param1 == 3, f"P23: expected Param1 3, got {param1!r}")


# --- P40/P46/P48: the doubled quotes come from CurrentText ---------------

def test_expr_p40_negative_result_rejected_by_the_range_check():
    """A legal expression whose value is negative fails the condact
    parameter range check (USintactic.pas:713), which reports
    CurrentText - still carrying its own quotes, hence the doubling."""
    _expr_error("P40", "-5",
                'Invalid parameter value ""-5"" for condact SET.')


def test_expr_p46_maxlongint_result_hits_the_sentinel():
    """Defect 19.56: a result of exactly 2147483647 is indistinguishable
    from the MAXLONGINT "no value" marker, so the condact parameter
    resolution chain runs to its end and reports the value as
    unresolvable."""
    _expr_error("P46", "2147483647",
                'Invalid parameter #1: ""2147483647"" for condact SET.')


def test_expr_p48_float_trunc_lands_on_the_sentinel():
    """The same collision reached through the float arm: trunc(2147483647.5)
    is 2147483647."""
    _expr_error("P48", "2147483647.5",
                'Invalid parameter #1: ""2147483647.5"" for condact SET.')


def test_expr_p47_maxlongint_result_at_a_define():
    """Defect 19.56 at the #define position: ExtractValue's first
    sentinel leg (USintactic.pas:145) fires instead."""
    _expr_error("P47", "2147483647",
                '""2147483647"" is not a valid expression.', mode="define")


# --- P43/P44: the 19.55 crashes, reproduced as a NAMED divergence -------

def test_expr_p43_out_of_longint_integer_is_a_named_divergence():
    """Defect 19.55: `Longint := <Int64>` under {$R+} (USintactic.pas:
    3,136) range-faults. The reference dies with an unhandled
    `ERangeError: Range check error`, exit 217, no diagnostic
    (live-confirmed 2026-08-27). Replicating an FPC crash dump serves
    nobody, so NDRC diagnoses it and names the reference behaviour;
    exit 2, not 217."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _expr_dsf(tmpdir, "5000000000")
        rc, out = _expr_run(tmpdir)
        check(rc == 2, f"P43: expected exit 2, got {rc}\n{out}")
        check("expression result 5000000000 is outside the 32-bit range" in out,
              f"P43: unexpected stdout {out!r}")
        check("ERangeError" in out and "217" in out,
              f"P43: the divergence must name the reference crash, got {out!r}")


def test_expr_p44_out_of_longint_float_is_a_named_divergence():
    """The float arm of the same crash: trunc(1e15) is in Int64 range,
    so the fault lands on the Longint store exactly as P43's does.
    Live-confirmed: `ERangeError: Range check error`, exit 217."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _expr_dsf(tmpdir, "1e15", mode="define")
        rc, out = _expr_run(tmpdir)
        check(rc == 2, f"P44: expected exit 2, got {rc}\n{out}")
        check(
            "expression result 1000000000000000 is outside the 32-bit range"
            in out,
            f"P44: unexpected stdout {out!r}",
        )
        check("ERangeError" in out and "217" in out,
              f"P44: the divergence must name the reference crash, got {out!r}")


# --- The two call-site trunc edges, measured this task -------------------

def test_expr_bare_1e19_is_the_call_site_trunc_crash():
    """`trunc(parserResult.ResFloat)` at USintactic.pas:135 sits OUTSIDE
    both TRY frames, so a Double beyond Int64 faults there uncaught -
    a SECOND crash class beside 19.55's, and a different exception:
    measured 2026-08-27 on operand `"1e19"`, the reference prints
    `An unhandled exception occurred at $00424EAC:` / `EInvalidOp:
    Invalid floating point operation` on STDERR with a frame dump and
    exits 217 (19.55's own probes give ERangeError instead). NDRC
    diagnoses and names it; exit 2."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _expr_dsf(tmpdir, "1e19")
        rc, out = _expr_run(tmpdir)
        check(rc == 2, f"1e19: expected exit 2, got {rc}\n{out}")
        check("outside the Int64 range" in out, f"1e19: unexpected stdout {out!r}")
        check("EInvalidOp" in out and "217" in out,
              f"1e19: the divergence must name the reference crash, got {out!r}")


def test_expr_trunc_1e19_is_caught_inside_the_evaluator():
    """The same hazard INSIDE evaluation is caught by the inner TRY and
    composed into the shell text. Measured 2026-08-27: exit 1,
    `447:37:g.DSF: Invalid expression "trunc(1e19)": Invalid floating
    point operation.` - the engine's own bound check on Round/Trunc's
    Int64 conversion."""
    _expr_error("trunc(1e19)", "trunc(1e19)",
                'Invalid expression "trunc(1e19)": Invalid floating point '
                'operation.')


# --- The two shells, end to end, against the live reference -------------

def _expr_shell_case(label, operand, expected):
    """Pins USintactic.pas:130/137's composition on the FULL line -
    location prefix, shell, inner engine text and SyntaxError's period -
    against the reference itself when one is configured, and against the
    transcribed text either way."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _expr_dsf(tmpdir, operand)
        rc, out = _expr_run(tmpdir)
        check(rc == 1, f"{label}: expected exit 1, got {rc}\n{out}")
        m = EXPR_LINE_RE.match(_last_line(out))
        check(m is not None, f"{label}: no diagnostic line in {out!r}")
        if m is not None:
            check(m.group(1) == expected,
                  f"{label}: expected {expected!r}, got {m.group(1)!r}")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir)
            check(ref_rc == rc,
                  f"{label}: exit {rc} but reference exits {ref_rc}")
            check(_last_line(out) == _last_line(ref_out),
                  f"{label}: ndrc {_last_line(out)!r} != reference "
                  f"{_last_line(ref_out)!r}")


def test_expr_shell_invalid_expression_composition():
    """P35: `Invalid expression "<stripped>": <engine text>` - the
    stripped text with NO quotes doubled, the engine's message verbatim,
    one period from SyntaxError."""
    _expr_shell_case(
        "P35 shell", "7 div 2",
        'Invalid expression "7 div 2": Badly terminated expression. Found '
        'token at position 6 : div.')


def test_expr_shell_non_numeric_composition():
    """P36: `Expression <stripped> returned a non numeric value` - no
    quotes at all around the text (USintactic.pas:137), unlike the shell
    above."""
    _expr_shell_case("P36 shell", "1=1",
                     "Expression 1=1 returned a non numeric value.")


# --------------------------------------------------------------------
# Include-boundary line-map pin: a syntax error
# INSIDE an included file must report the INCLUDE file's own name and
# its LOCAL line number - not the main file, and not the composite
# temp file Preparse (drf.pas:154-208/include.c) actually lexes from.
# SyntaxError threads every diagnostic through GetIncludeData
# (USintactic.pas:34-40/include.c's include_remap_for_diag), keyed on
# the temp file's line number, so this is what settles which of the
# three names wins - measured live against drf.exe 2026-08-27, not
# assumed: `1:17:BADVOC.INC: "blah" is not a valid
# vocabulary word type.` - the include file's bare name, its own line
# 1, column 17 ("blah" ends there in `ZZZINVALID 2 blah`); the
# composite temp file's own name is never printed anywhere.
# --------------------------------------------------------------------

def _include_variant_dsf(tmpdir: Path) -> Path:
    """BLANK_EN.DSF with a bare `#include BADVOC.INC` spliced into the
    existing /VOC section, right after its last real entry and before
    /STX. #include is a raw first-8-characters text splice (drf.pas:172,
    Preparse/include.c's is_include_line), transparent to section
    context, so the spliced-in vocabulary line is parsed as one more
    /VOC entry with no new section marker needed - a different
    placement than the committed INCLUDE.DSF fixture (which pulls in a
    whole /PRO block), chosen here to keep the deliberate error a single
    line. BADVOC.INC carries one deliberately invalid vocabulary line:
    `ZZZINVALID 2 blah` - "blah" is not a recognised word type
    (USintactic.pas:320/sintactic.c:583), so the diagnostic fires INSIDE
    the include, pinning which file/line gets reported."""
    src = DSF_FIXTURE.read_bytes().decode("latin-1")
    # Anchor on the line text alone and reuse whatever terminator
    # the checkout carries: the .DSF fixtures are not -text pinned,
    # so git hands out CRLF on Windows and LF elsewhere (the /END
    # splices above key on bare text for the same reason).
    marker = "THEN    2       conjugation"
    assert src.count(marker) == 1
    eol = "\r\n" if "\r\n" in src else "\n"
    src = src.replace(marker, marker + eol + "#include BADVOC.INC")
    dest = tmpdir / "g.dsf"
    dest.write_bytes(src.encode("latin-1"))
    (tmpdir / "BADVOC.INC").write_bytes(
        ("ZZZINVALID 2 blah" + eol).encode("latin-1"))
    return dest


def test_include_boundary_line_map_pin():
    """The include file's name and local line, never the main file or
    the composite temp file - against the live reference when
    configured (oracle.local.json), and against the transcribed text
    either way (Task 2's shell-case fallback idiom, _expr_shell_case
    above)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _include_variant_dsf(tmpdir)
        rc, out = _expr_run(tmpdir)
        check(rc == 1, f"include line-map: expected exit 1, got {rc}\n{out}")
        expected_full = (
            '1:17:BADVOC.INC: "blah" is not a valid vocabulary word type.\n'
        )
        check(_last_line(out) == expected_full,
              f"include line-map: expected {expected_full!r}, "
              f"got {_last_line(out)!r}")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir)
            check(ref_rc == rc,
                  f"include line-map: ndrc exit {rc} but reference exits "
                  f"{ref_rc}")
            check(_last_line(out) == _last_line(ref_out),
                  f"include line-map: ndrc {_last_line(out)!r} != reference "
                  f"{_last_line(ref_out)!r}")


# --------------------------------------------------------------------
# The two SEMANTIC flags, -no-semantic and -semantic-warnings.
#
# Neither can be observable on a committed fixture: every fixture
# compiles clean by definition, so semantic analysis has nothing to
# reject and both flags are indistinguishable from their own absence.
# They need a DELIBERATELY-INVALID variant instead - the same
# generate-and-run-both idiom the quoted-expression cases above use -
# and that is why these two live here rather than among verify.py's
# --to-json flag extras.
#
# The construct: `MESSAGE 200` in a spliced `/PRO 7`. It is
# SYNTACTICALLY fine (MESSAGE takes one mesno parameter, and 200 is
# inside the 0-255 parameter range USintactic.pas:716 enforces), so it
# gets all the way to the semantic check at USintactic.pas:720-728,
# where UCondacts.pas:256's `mesno: IF ParamValue >= MTXCount` rejects
# it - BLANK_EN.DSF's /MTX holds exactly one message, so 200 does not
# exist. That is the whole point: the three legs below differ ONLY in
# how that one check is treated.
#
# All three legs MEASURED against reference drf.exe 0.40 on 2026-08-27:
#
#   bare                 exit 1, "446:27:g.dsf: Message 200 does not
#                        exist." as the last line, NO out.json written.
#   -no-semantic         exit 0, the normal five stage lines, out.json
#                        written with the RAW value: process 7's condact
#                        is {"Opcode":38,"Condact":"MESSAGE",
#                        "Indirection1":0,"Param1":200,"NumParams":1}.
#   -semantic-warnings   exit 0, the SAME out.json as -no-semantic (byte
#                        for byte), plus one extra line "Warning:
#                        446:27:g.dsf: Message 200 does not exist."
#                        between "Checking Syntax..." and "Updating
#                        forward references..." - Warning() at
#                        USintactic.pas:42-47 prints the identical
#                        location-and-text string SyntaxError does, with
#                        a "Warning: " prefix and no exit.
#
# The line:column prefix is checked by shape rather than transcribed, as
# the expression cases do, so a fixture edit cannot silently invalidate
# a text; the reference comparison pins it byte for byte when a
# reference is configured.
# --------------------------------------------------------------------

SEMANTIC_ERROR_TEXT = "Message 200 does not exist."
WARNING_LINE_RE = re.compile(r"^Warning: \d+:\d+:g\.dsf: (.*)\n\Z")


def _semantic_dsf(tmpdir: Path) -> Path:
    """BLANK_EN.DSF + one semantically-invalid condact, as g.dsf.

    Same splice shape as _expr_dsf's "condact" mode (a `/PRO 7` holding
    a single entry, inserted just before /END), with MESSAGE instead of
    SET so the parameter reaches a semantic check that can reject it."""
    src = DSF_FIXTURE.read_bytes().decode("latin-1")
    body = "\n/PRO 7\n> _       _     MESSAGE 200\n\n"
    assert src.count("/END") == 1
    src = src.replace("/END", body + "/END")
    dest = tmpdir / "g.dsf"
    dest.write_bytes(src.encode("latin-1"))
    return dest


def _semantic_param1(tmpdir: Path):
    """Process 7's single condact from the out.json just written, or
    None if there is no out.json to read."""
    doc_path = tmpdir / "out.json"
    if not doc_path.exists():
        return None
    doc = json.loads(doc_path.read_text(encoding="latin-1"))
    proc = _find_process(doc, 7)
    if proc is None:
        return None
    return proc["entries"][0]["condacts"][0]


def test_to_json_semantic_bare_rejects_the_invalid_construct():
    """The baseline the two flags are defined against: with neither
    flag, SemanticCheck's rejection is a SyntaxError - exit 1, the
    diagnostic as the last line, and no JSON written at all."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _semantic_dsf(tmpdir)
        rc, out = _expr_run(tmpdir)
        check(rc == 1, f"semantic bare: expected exit 1, got {rc}\n{out}")
        m = EXPR_LINE_RE.match(_last_line(out))
        check(m is not None,
              f"semantic bare: no `<line>:<col>:g.dsf: ...` line in {out!r}")
        if m is not None:
            check(m.group(1) == SEMANTIC_ERROR_TEXT,
                  f"semantic bare: expected {SEMANTIC_ERROR_TEXT!r}, "
                  f"got {m.group(1)!r}")
        check(not (tmpdir / "out.json").exists(),
              "semantic bare: no out.json must be written on a rejection")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir)
            check(ref_rc == rc,
                  f"semantic bare: ndrc exit {rc} but reference exits {ref_rc}")
            check(_last_line(out) == _last_line(ref_out),
                  f"semantic bare: ndrc {_last_line(out)!r} != reference "
                  f"{_last_line(ref_out)!r}")


def test_to_json_no_semantic_compiles_the_raw_value():
    """-no-semantic (drf.pas:362-366 -> NoSemantic -> the guard at
    USintactic.pas:720): the check never runs, so the out-of-range value
    survives into the JSON verbatim and the compile succeeds silently -
    no diagnostic, no warning, the ordinary stage lines."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _semantic_dsf(tmpdir)
        rc, out = _expr_run(tmpdir, ("-no-semantic",))
        check(rc == 0, f"-no-semantic: expected exit 0, got {rc}\n{out}")
        expected = (
            "Generating DAAD V3 DDB\n"
            "Reading g.dsf\n"
            "Checking Syntax...\n"
            "Updating forward references...\n"
            "Generating out.json [Classic mode OFF]\n"
            "out.json generated.\n"
        )
        check(out == expected, f"-no-semantic: unexpected stdout {out!r}")
        condact = _semantic_param1(tmpdir)
        check(condact is not None,
              "-no-semantic: expected out.json with a process 7")
        if condact is not None:
            check(condact.get("Condact") == "MESSAGE"
                  and condact.get("Param1") == 200,
                  f"-no-semantic: expected MESSAGE Param1 200, got {condact!r}")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir, ("-no-semantic",))
            ref_condact = _semantic_param1(tmpdir)
            check(ref_rc == rc,
                  f"-no-semantic: ndrc exit {rc} but reference exits {ref_rc}")
            check(out == ref_out,
                  f"-no-semantic: ndrc stdout {out!r} != reference {ref_out!r}")
            check(ref_condact == condact,
                  f"-no-semantic: ndrc condact {condact!r} != reference "
                  f"{ref_condact!r}")


def test_to_json_semantic_warnings_warns_and_exits_zero():
    """-semantic-warnings (drf.pas:367-371 -> SemanticWarnings ->
    USintactic.pas:725's Warning branch): the check DOES run and does
    reject, but its text is printed as a warning and compilation carries
    on to the same JSON -no-semantic produces."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _semantic_dsf(tmpdir)
        rc, out = _expr_run(tmpdir, ("-semantic-warnings",))
        check(rc == 0, f"-semantic-warnings: expected exit 0, got {rc}\n{out}")
        lines = out.splitlines(keepends=True)
        warnings = [ln for ln in lines if ln.startswith("Warning: ")]
        check(len(warnings) == 1,
              f"-semantic-warnings: expected exactly one Warning line, "
              f"got {warnings!r}")
        if warnings:
            m = WARNING_LINE_RE.match(warnings[0])
            check(m is not None,
                  f"-semantic-warnings: malformed warning {warnings[0]!r}")
            if m is not None:
                check(m.group(1) == SEMANTIC_ERROR_TEXT,
                      f"-semantic-warnings: expected {SEMANTIC_ERROR_TEXT!r}, "
                      f"got {m.group(1)!r}")
            # The warning is emitted mid-parse, so it lands between the
            # syntax pass and the forward-reference pass - not appended
            # at the end the way a SyntaxError's own last line is.
            idx = lines.index(warnings[0])
            check(lines[idx - 1] == "Checking Syntax...\n"
                  and lines[idx + 1] == "Updating forward references...\n",
                  f"-semantic-warnings: warning out of position in {out!r}")
        condact = _semantic_param1(tmpdir)
        check(condact is not None,
              "-semantic-warnings: expected out.json with a process 7")
        if condact is not None:
            check(condact.get("Condact") == "MESSAGE"
                  and condact.get("Param1") == 200,
                  f"-semantic-warnings: expected MESSAGE Param1 200, "
                  f"got {condact!r}")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir, ("-semantic-warnings",))
            ref_condact = _semantic_param1(tmpdir)
            check(ref_rc == rc,
                  f"-semantic-warnings: ndrc exit {rc} but reference exits "
                  f"{ref_rc}")
            check(out == ref_out,
                  f"-semantic-warnings: ndrc stdout {out!r} != reference "
                  f"{ref_out!r}")
            check(ref_condact == condact,
                  f"-semantic-warnings: ndrc condact {condact!r} != "
                  f"reference {ref_condact!r}")


# --------------------------------------------------------------------
# The positional SYMBOL LIST - the trailing `CLIONLY,SECOND` argument.
#
# drf.pas:354 takes any trailing argument not starting with '-' (and
# with no dot in it - see the heuristic cases above) as AdditionalSymbols
# WHOLESALE, overwriting whatever a previous one set, and drf.pas:292-300
# then walks it with `ExtractWord(i, AdditionalSymbols, [','])`,
# adding each non-empty word with the LOOP COUNTER i as its value.
#
# MEASURED against reference drf.exe 0.40 on 2026-08-27, and this is
# where the brief's own expectation needed correcting: ExtractWord
# COLLAPSES runs of delimiters rather than yielding an empty word per
# empty slot, so "A,,B" gives B the value 2, NOT 3 - there are no empty
# slots to number, and a leading or trailing comma costs nothing. The
# loop's terminating condition (AuxString = '') is reached only past the
# last word, so it cannot stop early on an interior gap either.
#
# The comma is the ONLY delimiter: ';' and ' ' are ordinary name
# characters, so "A;B" and "A B" each define ONE symbol whose name
# contains them. Names fold to upper case on the way in (AddSymbol), as
# the accented-symbol case above already pins for the cp1252 range.
# --------------------------------------------------------------------

SYMBOL_LIST_CASES = (
    ("CLIONLY,SECOND", {"CLIONLY": 1, "SECOND": 2}),
    ("A", {"A": 1}),
    # Empty slots collapse - B is 2, not 3.
    ("A,,B", {"A": 1, "B": 2}),
    ("A,,,B,C", {"A": 1, "B": 2, "C": 3}),
    # A leading or trailing comma is not a slot either.
    (",A,B", {"A": 1, "B": 2}),
    ("A,B,", {"A": 1, "B": 2}),
    # Case folds.
    ("a,b", {"A": 1, "B": 2}),
    # Comma is the only separator: these are single symbols whose names
    # contain a semicolon and a space respectively.
    ("A;B", {"A;B": 1}),
    ("A B", {"A B": 1}),
)


def _symbol_values(tmpdir: Path, names) -> dict:
    """The {name: Value} the just-written out.json defines for `names`,
    omitting any that is not defined at all."""
    doc = json.loads((tmpdir / "out.json").read_text(encoding="latin-1"))
    wanted = set(names)
    return {s["symbol"]: s["Value"] for s in doc["symbols"]
            if s["symbol"] in wanted}


def test_to_json_symbol_list_slots_and_separators():
    """One compile per SYMBOL_LIST_CASES row, checking the exact
    {name: value} the list defines - against the reference too when one
    is configured."""
    for arg, expected in SYMBOL_LIST_CASES:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            _dsf_copy(tmpdir)
            rc, out = _expr_run(tmpdir, (arg,))
            check(rc == 0,
                  f"symbol list {arg!r}: expected exit 0, got {rc}\n{out}")
            if rc != 0:
                continue
            got = _symbol_values(tmpdir, expected)
            check(got == expected,
                  f"symbol list {arg!r}: expected {expected}, got {got}")
            if _drf_path() is not None:
                ref_rc, _ref_out = _ref_run(tmpdir, (arg,))
                check(ref_rc == rc,
                      f"symbol list {arg!r}: ndrc exit {rc} but reference "
                      f"exits {ref_rc}")
                if ref_rc == 0:
                    ref_got = _symbol_values(tmpdir, expected)
                    check(ref_got == got,
                          f"symbol list {arg!r}: ndrc {got} != reference "
                          f"{ref_got}")


def test_to_json_symbol_list_flips_an_ifdef():
    """What the symbol list is FOR: IFDEFS.DSF:475-478 guards a `SET 17`
    behind `#ifdef "CLIONLY"`, and nothing in the fixture or in DRF's
    built-in symbols defines CLIONLY - only the command line can.

    MEASURED on IFDEFS/NEXTDAAD/-v3: without a list, process 7's SET
    parameters are [10, 11, 14]; with `CLIONLY,SECOND` they are
    [10, 11, 14, 17], and the symbol table gains CLIONLY=1 and SECOND=2.
    SECOND guards nothing in the fixture and is present only to pin that
    the second slot's value is 2."""
    for arg, expected_sets, expected_syms in (
        ((), [10, 11, 14], {}),
        (("CLIONLY,SECOND",), [10, 11, 14, 17],
         {"CLIONLY": 1, "SECOND": 2}),
    ):
        label = f"ifdef flip {arg!r}"
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            shutil.copyfile(IFDEFS_DSF_FIXTURE, tmpdir / "g.dsf")
            rc, out = _expr_run(tmpdir, arg)
            check(rc == 0, f"{label}: expected exit 0, got {rc}\n{out}")
            if rc != 0:
                continue
            doc = json.loads(
                (tmpdir / "out.json").read_text(encoding="latin-1"))
            proc = _find_process(doc, 7)
            check(proc is not None, f"{label}: no process 7 in the JSON")
            if proc is not None:
                sets = [c["Param1"] for e in proc["entries"]
                        for c in e["condacts"] if c.get("Condact") == "SET"]
                check(sets == expected_sets,
                      f"{label}: expected SET params {expected_sets}, "
                      f"got {sets}")
            syms = _symbol_values(tmpdir, ("CLIONLY", "SECOND"))
            check(syms == expected_syms,
                  f"{label}: expected symbols {expected_syms}, got {syms}")
            if _drf_path() is not None:
                ref_rc, _ref_out = _ref_run(tmpdir, arg)
                check(ref_rc == rc,
                      f"{label}: ndrc exit {rc} but reference exits {ref_rc}")
                if ref_rc == 0:
                    ref_syms = _symbol_values(tmpdir, ("CLIONLY", "SECOND"))
                    check(ref_syms == syms,
                          f"{label}: ndrc {syms} != reference {ref_syms}")


# --------------------------------------------------------------------
# X-condact handling: -replace-xcondacts (controller Ruling 8) - the
# sintactic port of USintactic.pas:581-602, which no committed fixture
# reaches. verify.py's own
# tojson_x_CONDACTS_replace_xcondacts row (TO_JSON_EXTRA_JOBS) measured
# the flag as a complete no-op on CONDACTS/NEXTDAAD/-v3: the guarded
# block only fires on XPICTURE, XSAVE, XLOAD and XBEEP, and none of the
# 14 committed .DSF fixtures use any of the four - so that row pins the
# no-op, not the rewrite/rejection this flag actually performs. A
# BLANK_EN variant with one `XPICTURE 0` condact spliced into a fresh
# /PRO 7 (same splice shape as _semantic_dsf's own /PRO 7) reaches the
# guarded block directly.
#
# MEASURED against reference drf.exe 0.40 on 2026-08-27:
#   bare                  exit 0, compiles clean - XPICTURE is an
#                         ordinary, unguarded condact until the flag is
#                         given, so nothing here rejects it.
#   -replace-xcondacts    exit 1, last stdout line "446:24:g.dsf:
#                         XPICTURE cannot be used in this target
#                         [NEXTDAAD]..\n" - no out.json written. The
#                         doubled period is the reference's own (its
#                         "[TARGET]." interpolation followed by
#                         SyntaxError's own trailing "."), not a
#                         transcription slip.
# --------------------------------------------------------------------

XPICTURE_ERROR_LAST_LINE = (
    "446:24:g.dsf: XPICTURE cannot be used in this target [NEXTDAAD]..\n"
)


def _xpicture_dsf(tmpdir: Path) -> Path:
    """BLANK_EN.DSF + one `XPICTURE 0` condact in a spliced /PRO 7, as
    g.dsf. Same splice shape as _semantic_dsf's own /PRO 7 - a single
    entry inserted just before /END."""
    src = DSF_FIXTURE.read_bytes().decode("latin-1")
    body = "\n/PRO 7\n> _       _     XPICTURE 0\n\n"
    assert src.count("/END") == 1
    src = src.replace("/END", body + "/END")
    dest = tmpdir / "g.dsf"
    dest.write_bytes(src.encode("latin-1"))
    return dest


def test_to_json_xpicture_bare_compiles_clean():
    """Leg 1: exit 0. Leg 2: the ordinary five stage lines, unchanged by
    XPICTURE's presence. Leg 3: matched against a live reference run
    when one is configured (no-reference fallback, same idiom as the
    semantic-flag cases above)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _xpicture_dsf(tmpdir)
        rc, out = _expr_run(tmpdir)
        check(rc == 0, f"xpicture bare: expected exit 0, got {rc}\n{out}")
        expected = (
            "Generating DAAD V3 DDB\n"
            "Reading g.dsf\n"
            "Checking Syntax...\n"
            "Updating forward references...\n"
            "Generating out.json [Classic mode OFF]\n"
            "out.json generated.\n"
        )
        check(out == expected, f"xpicture bare: unexpected stdout {out!r}")
        check((tmpdir / "out.json").exists(),
              "xpicture bare: expected out.json to be written")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir)
            check(ref_rc == rc,
                  f"xpicture bare: ndrc exit {rc} but reference exits "
                  f"{ref_rc}")
            check(out == ref_out,
                  f"xpicture bare: ndrc stdout {out!r} != reference "
                  f"{ref_out!r}")


def test_to_json_xpicture_replace_xcondacts_rejects_the_target():
    """-replace-xcondacts (drf.pas:405-409 -> replace_xcondacts ->
    USintactic.pas:581-602): XPICTURE's own arm sets MaluvaUsed on the
    four 8-bit targets it is valid for and SyntaxErrors everywhere else -
    NEXTDAAD is not among them, so this compile fails.

    Leg 1: exit 1. Leg 2: the exact pinned last stdout line (not shape-
    matched - the brief calls for the full line, doubled period
    included). Leg 3: matched against a live reference run when
    configured."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _xpicture_dsf(tmpdir)
        rc, out = _expr_run(tmpdir, ("-replace-xcondacts",))
        check(rc == 1,
              f"xpicture -replace-xcondacts: expected exit 1, got {rc}\n"
              f"{out}")
        check(_last_line(out) == XPICTURE_ERROR_LAST_LINE,
              f"xpicture -replace-xcondacts: expected "
              f"{XPICTURE_ERROR_LAST_LINE!r}, got {_last_line(out)!r}")
        check(not (tmpdir / "out.json").exists(),
              "xpicture -replace-xcondacts: no out.json must be written "
              "on a rejection")
        if _drf_path() is not None:
            ref_rc, ref_out = _ref_run(tmpdir, ("-replace-xcondacts",))
            check(ref_rc == rc,
                  f"xpicture -replace-xcondacts: ndrc exit {rc} but "
                  f"reference exits {ref_rc}")
            check(_last_line(out) == _last_line(ref_out),
                  f"xpicture -replace-xcondacts: ndrc "
                  f"{_last_line(out)!r} != reference "
                  f"{_last_line(ref_out)!r}")


# --------------------------------------------------------------------
# The JOIN: bare `ndrc TARGET [SUBTARGET] LANG in.DSF [out.ddb]
# [symbols] [options]` - one process running the drf stage then the drb
# stage, reproducing the reference two-program flow's concatenated
# transcript, its exit code and its DDB bytes.
#
# MEASURED 2026-08-27 against the reference flow itself (drf.exe, then
# drb.php on the JSON drf left behind), each half banner-stripped and
# concatenated in order:
#   NEXTDAAD EN BLANK_EN -v3  both stages exit 0; drf writes g.json and
#                             drb writes g.DDB, 2038 bytes
#   BOGUS EN                  drf exits 0 (defect 19.3 - an
#                             unrecognised target compiles silently at
#                             COLS=42/ROWS=25), then drb prints
#                             "Error: Invalid target machine 'BOGUS'."
#                             and exits 2, with NO Target: line
#   NEXTDAAD XX               drf exits 0, then drb prints
#                             "Target: NEXTDAAD" and "Error: Invalid
#                             target language.", exit 2
#   ZX EN g.dsf               drf exits 2 at the subtarget check (EN
#                             lands in the mandatory subtarget slot);
#                             drb never runs
#   TOKFILE.DSF + .tok        drb's verbose echo is "Loading tokens
#                             from .\TOKFILE.tok." - keyed on the
#                             flow's JSON name, which is what the join
#                             hands its drb stage
#   XMSG.DSF                  drf exits 0, drb refuses with "Error:
#                             There is not data enough in XDATA
#                             condact.", exit 2
#
# The join's own JSON name is always drf's default <input>.json (no
# dirname prefix): it drives the Generating/generated echoes, the drb
# stage's default DDB name and its .tok candidate. --json[=path] is a
# tee that never moves it.
# --------------------------------------------------------------------

JOIN_DDB_GOLDENS = HERE.parent / "goldens"


def _php_drb():
    """(php, drb.php) from oracle.local.json, or None. The reference
    FLOW needs both halves plus _drf_path(); like every other
    reference-backed case here, each join case still asserts on its own
    when they are missing."""
    cfg = HERE / "oracle.local.json"
    if not cfg.exists():
        return None
    try:
        raw = json.loads(cfg.read_text(encoding="utf-8"))
    except ValueError:
        return None
    php, drb = Path(raw.get("php", "")), Path(raw.get("drb", ""))
    return (php, drb) if php.exists() and drb.exists() else None


def _flow_available():
    return _drf_path() is not None and _php_drb() is not None


def _banner_stripped(raw: bytes) -> str:
    """One reference stage's stdout, CRLF-normalised with its own banner
    line dropped - the same treatment run() gives ndrc's."""
    text = raw.decode("latin-1").replace("\r\n", "\n")
    lines = text.splitlines(keepends=True)
    return "".join(lines[1:])


def _flow_run(tmpdir: Path, drf_args, drb_args):
    """The reference two-program flow in `tmpdir`: drf.exe, then drb.php
    on the JSON it left behind. Returns (exit code, concatenated
    banner-stripped transcript). drb is not run when drf fails - the
    join aborts at the same point, so the flow must too."""
    php, drb = _php_drb()
    p1 = subprocess.run([str(_drf_path()), *drf_args],
                        capture_output=True, cwd=str(tmpdir))
    out = _banner_stripped(p1.stdout)
    if p1.returncode != 0:
        return p1.returncode, out
    p2 = subprocess.run([str(php), str(drb), *drb_args],
                        capture_output=True, cwd=str(tmpdir))
    return p2.returncode, out + _banner_stripped(p2.stdout)


def _clock_normaliser():
    """verify.py's own _normalise_clock_symbols - IMPORTED, never
    reimplemented: both files live here and one normaliser serves every
    JSON gate."""
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    from verify import _normalise_clock_symbols
    return _normalise_clock_symbols


def _join_dir(tmp, fixture=DSF_FIXTURE, name="g.dsf") -> Path:
    tmpdir = Path(tmp)
    shutil.copyfile(fixture, tmpdir / name)
    return tmpdir


JOIN_DRF_STAGE_V3 = (
    "Generating DAAD V3 DDB\n"
    "Reading g.dsf\n"
    "Checking Syntax...\n"
    "Updating forward references...\n"
    "Generating g.json [Classic mode OFF]\n"
    "g.json generated.\n"
)


def test_join_happy_path_reproduces_the_flow():
    """Three legs: the DDB bytes (against the committed golden, so this
    holds with no reference toolchain), the concatenated transcript, and
    the exit code. With a reference configured, all three are compared
    against the live flow as well."""
    golden = JOIN_DDB_GOLDENS / "BLANK_EN" / "NEXTDAAD_EN_v3_opt.ddb"
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-v3"], cwd=str(tmpdir))
        check(rc == 0, f"join happy: expected exit 0, got {rc}\n{out}")
        check(out.startswith(JOIN_DRF_STAGE_V3),
              f"join happy: drf-stage prefix missing, got {out!r}")
        drb_half = out[len(JOIN_DRF_STAGE_V3):]
        check(drb_half.startswith("Target: NEXTDAAD\n"),
              f"join happy: drb stage must open with its Target line, "
              f"got {drb_half!r}")
        check(drb_half.endswith("Text compression savings: 272 bytes.\n"),
              f"join happy: unexpected drb-stage tail {drb_half!r}")
        built = tmpdir / "g.DDB"
        check(built.exists(), "join happy: expected g.DDB to be written")
        check(not (tmpdir / "g.json").exists(),
              "join happy: no JSON file without --json")
        if not built.exists():
            return
        built_bytes = built.read_bytes()
        if golden.exists():
            check(built_bytes == golden.read_bytes(),
                  "join happy: DDB bytes differ from the committed golden")
        if not _flow_available():
            return
        with tempfile.TemporaryDirectory() as reftmp:
            refdir = _join_dir(reftmp)
            ref_rc, ref_out = _flow_run(refdir, ("NEXTDAAD", "g.dsf", "-v3"),
                                        ("NEXTDAAD", "EN", "g.json"))
            check(ref_rc == 0, f"join happy: reference flow exited {ref_rc}")
            check(ref_out == out,
                  f"join happy: transcript {out!r} != flow {ref_out!r}")
            ref_ddb = refdir / "g.DDB"
            check(ref_ddb.exists() and ref_ddb.read_bytes() == built_bytes,
                  "join happy: DDB bytes differ from the reference flow")


def test_join_bogus_target_dies_at_the_drb_stage():
    """Defect 19.3 end to end: the drf stage compiles an unrecognised
    target silently, and only the drb stage refuses it - so the whole
    drf transcript precedes the error, and no Target: line is printed."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["BOGUS", "EN", "g.dsf"], cwd=str(tmpdir))
        check(rc == 2, f"join bogus: expected exit 2, got {rc}\n{out}")
        expected = (
            "Reading g.dsf\n"
            "Checking Syntax...\n"
            "Updating forward references...\n"
            "Generating g.json [Classic mode OFF]\n"
            "g.json generated.\n"
            "Error: Invalid target machine 'BOGUS'.\n"
        )
        check(out == expected, f"join bogus: unexpected stdout {out!r}")
        check(not (tmpdir / "g.DDB").exists(),
              "join bogus: no DDB on a drb-stage refusal")
        if _flow_available():
            with tempfile.TemporaryDirectory() as reftmp:
                ref_rc, ref_out = _flow_run(_join_dir(reftmp),
                                            ("BOGUS", "g.dsf"),
                                            ("BOGUS", "EN", "g.json"))
            check(ref_rc == rc,
                  f"join bogus: ndrc exit {rc} but flow exits {ref_rc}")
            check(ref_out == out,
                  f"join bogus: transcript {out!r} != flow {ref_out!r}")


def test_join_invalid_language_errors_at_the_drb_position():
    """The LANG slot is unvalidated at parse time; the drb stage rejects
    it after its own Target line, at the flow's position."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["NEXTDAAD", "XX", "g.dsf"], cwd=str(tmpdir))
        check(rc == 2, f"join bad lang: expected exit 2, got {rc}\n{out}")
        expected = (
            "Reading g.dsf\n"
            "Checking Syntax...\n"
            "Updating forward references...\n"
            "Generating g.json [Classic mode OFF]\n"
            "g.json generated.\n"
            "Target: NEXTDAAD\n"
            "Error: Invalid target language.\n"
        )
        check(out == expected, f"join bad lang: unexpected stdout {out!r}")
        if _flow_available():
            with tempfile.TemporaryDirectory() as reftmp:
                ref_rc, ref_out = _flow_run(_join_dir(reftmp),
                                            ("NEXTDAAD", "g.dsf"),
                                            ("NEXTDAAD", "XX", "g.json"))
            check(ref_rc == rc,
                  f"join bad lang: ndrc exit {rc} but flow exits {ref_rc}")
            check(ref_out == out,
                  f"join bad lang: transcript {out!r} != flow {ref_out!r}")


def test_join_zx_without_subtarget_shifts_the_arguments():
    """ZX demands a subtarget, so `ZX EN g.dsf` puts EN in the subtarget
    slot and the drf stage refuses it - inherited from --to-json's own
    grammar, deliberately (design section 2). No drb-stage output."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["ZX", "EN", "g.dsf"], cwd=str(tmpdir))
        check(rc == 2, f"join zx shift: expected exit 2, got {rc}\n{out}")
        check(
            out == (
                '"EN" is not a valid subtarget for target "ZX". Please '
                "specify a valid subtarget. Call DRF without parameters "
                "for more information..\n"
            ),
            f"join zx shift: unexpected stdout {out!r}",
        )


def test_join_unknown_option_is_a_param_error():
    """An option in neither stage's set: ndrc's existing ParamError
    shape (bare text, exit 2), raised before any compilation."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-bogus"], cwd=str(tmpdir))
        check(rc == 2, f"join bad option: expected exit 2, got {rc}\n{out}")
        check(out == "Invalid option: -bogus.\n",
              f"join bad option: unexpected stdout {out!r}")


def test_join_too_few_args_takes_the_usage_class():
    """CONTROLLER RULING: the join's too-few-args takes --to-json's
    deliberate usage class (usage text, exit 1), NOT ParamError's exit
    2. Three positionals - target, language, input - are the minimum."""
    rc, out = run(["NEXTDAAD", "EN"])
    check(rc == 1, f"join too few args: expected exit 1, got {rc}\n{out}")
    check(
        out == (
            "usage: ndrc TARGET [SUBTARGET] LANG file.dsf [output.ddb] "
            "[symbols] [options]\n"
        ),
        f"join too few args: unexpected usage line {out!r}",
    )


def test_join_no_args_keeps_the_pre_join_usage_error():
    """A bare `ndrc` with no arguments at all is NOT the join: it keeps
    the --from-json usage error it printed before the join existed."""
    rc, out = run([])
    check(rc == 2, f"join no args: expected exit 2, got {rc}\n{out}")
    check(out.startswith("Error: usage: ndrc --from-json"),
          f"join no args: unexpected stdout {out!r}")


def test_join_json_tee_matches_to_json_byte_for_byte():
    """--json bare and --json=<path> both write exactly what --to-json
    writes for the same DSF/target/options, after the shared clock
    normaliser runs on both sides (the two runs are separate processes
    and the four clock symbols are date-bearing). The bare case also
    pins the default name: <input>.json via drf's change_file_ext
    naming, with NO dirname prefix (unlike the DDB's replace_extension,
    which prefixes "./")."""
    _normalise_clock_symbols = _clock_normaliser()

    def _norm(p: Path):
        if not p.exists():
            return None
        return _normalise_clock_symbols(p.read_text(encoding="latin-1"))

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["--to-json", "NEXTDAAD", "g.dsf", "ref.json", "-v3"],
                      cwd=str(tmpdir))
        check(rc == 0, f"json tee: --to-json exited {rc}\n{out}")
        reference = _norm(tmpdir / "ref.json")
        check(reference is not None, "json tee: --to-json wrote no ref.json")

        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-v3", "--json"],
                      cwd=str(tmpdir))
        check(rc == 0, f"json tee: bare --json exited {rc}\n{out}")
        check((tmpdir / "g.json").exists(),
              "json tee: bare --json must write <input>.json beside it")
        check(_norm(tmpdir / "g.json") == reference,
              "json tee: bare --json bytes differ from --to-json's")

        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "out.ddb", "-v3",
                       "--json=named.json"], cwd=str(tmpdir))
        check(rc == 0, f"json tee: --json=path exited {rc}\n{out}")
        check(_norm(tmpdir / "named.json") == reference,
              "json tee: --json=path bytes differ from --to-json's")
        check("Generating g.json [Classic mode OFF]\n" in out,
              f"json tee: the tee must not move the flow's JSON name, "
              f"got {out!r}")


def test_join_json_tee_survives_a_drb_stage_failure():
    """The tee is written immediately after the drf stage, so a drb
    refusal (XMSG's XDATA check) still leaves the diagnostic JSON, equal
    to --to-json's output post-normalisation."""
    _normalise_clock_symbols = _clock_normaliser()

    xmsg = HERE.parent / "fixtures" / "XMSG.DSF"
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp, fixture=xmsg)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-v3", "--json"],
                      cwd=str(tmpdir))
        check(rc == 2, f"json tee/XMSG: expected exit 2, got {rc}\n{out}")
        check(out.endswith(
                  "Error: There is not data enough in XDATA condact.\n"),
              f"json tee/XMSG: unexpected stdout {out!r}")
        teed = tmpdir / "g.json"
        check(teed.exists(),
              "json tee/XMSG: the tee must survive the drb-stage failure")
        if not teed.exists():
            return
        kept = _normalise_clock_symbols(teed.read_text(encoding="latin-1"))
        teed.unlink()
        rc, out = run(["--to-json", "NEXTDAAD", "g.dsf", "g.json", "-v3"],
                      cwd=str(tmpdir))
        check(rc == 0, f"json tee/XMSG: --to-json exited {rc}\n{out}")
        check(_normalise_clock_symbols(
                  teed.read_text(encoding="latin-1")) == kept,
              "json tee/XMSG: teed bytes differ from --to-json's")


def test_join_json_tee_unwritable_path():
    """An unwritable --json path is diagnosed with the same shape
    --to-json uses for an unwritable output - both pinned here, since
    the two texts live in different files and must not drift. The
    failure lands between the Generating and generated lines, aborting
    before the drb stage."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["--to-json", "NEXTDAAD", "g.dsf", "nodir/out.json"],
                      cwd=str(tmpdir))
        check(rc == 2, f"unwritable --to-json: expected exit 2, got {rc}")
        check(out.endswith(
                  'Generating nodir/out.json [Classic mode OFF]\n'
                  'Error: cannot open "nodir/out.json" for writing.\n'),
              f"unwritable --to-json: unexpected stdout {out!r}")

        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "--json=nodir/out.json"],
                      cwd=str(tmpdir))
        check(rc == 2, f"unwritable --json: expected exit 2, got {rc}")
        check(out.endswith(
                  'Generating g.json [Classic mode OFF]\n'
                  'Error: cannot open "nodir/out.json" for writing.\n'),
              f"unwritable --json: unexpected stdout {out!r}")
        check(not (tmpdir / "g.DDB").exists(),
              "unwritable --json: the drb stage must not have run")


def test_join_tok_sidecar_is_discovered():
    """.tok discovery keys off the flow's JSON name, so a TOKFILE.tok
    beside TOKFILE.DSF is found exactly as the flow finds it beside
    TOKFILE.json - measured echo: "Loading tokens from .\\TOKFILE.tok.".
    A --json pointing elsewhere does NOT move the search."""
    tok_dsf = HERE.parent / "fixtures" / "TOKFILE.DSF"
    tok_side = HERE.parent / "fixtures" / "TOKFILE.tok"
    golden = JOIN_DDB_GOLDENS / "TOKFILE" / "NEXTDAAD_EN_v3_opt.ddb"
    for extra, label in ((), "bare"), (("--json=aside.json",), "tee elsewhere"):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            shutil.copyfile(tok_dsf, tmpdir / "TOKFILE.DSF")
            shutil.copyfile(tok_side, tmpdir / "TOKFILE.tok")
            rc, out = run(["NEXTDAAD", "EN", "TOKFILE.DSF", "-v3", "-v",
                           *extra], cwd=str(tmpdir))
            check(rc == 0, f"tok {label}: expected exit 0, got {rc}\n{out}")
            check("Loading tokens from .\\TOKFILE.tok.\n" in out,
                  f"tok {label}: override echo missing from {out!r}")
            built = tmpdir / "TOKFILE.DDB"
            check(built.exists(), f"tok {label}: expected TOKFILE.DDB")
            if built.exists() and golden.exists():
                check(built.read_bytes() == golden.read_bytes(),
                      f"tok {label}: DDB bytes differ from the golden")


def test_join_option_order_changes_the_drf_stage_stdout():
    """Defect 19.19 through the join: -verbose gates each option's own
    confirmation line at the moment that option is parsed, so the two
    orders print different transcripts. Compared against the reference
    flow when one is configured."""
    orders = (("-no-semantic", "-verbose"), ("-verbose", "-no-semantic"))
    seen = []
    for opts in orders:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = _join_dir(tmp)
            rc, out = run(["NEXTDAAD", "EN", "g.dsf", *opts], cwd=str(tmpdir))
            check(rc == 0, f"19.19 {opts}: expected exit 0, got {rc}\n{out}")
            seen.append(out)
            if _flow_available():
                with tempfile.TemporaryDirectory() as reftmp:
                    ref_rc, ref_out = _flow_run(
                        _join_dir(reftmp),
                        ("NEXTDAAD", "g.dsf", *opts),
                        ("NEXTDAAD", "EN", "g.json"))
                check(ref_rc == rc,
                      f"19.19 {opts}: ndrc exit {rc} but flow exits {ref_rc}")
                check(ref_out == out,
                      f"19.19 {opts}: transcript {out!r} != flow {ref_out!r}")
    check(seen[0] != seen[1],
          "19.19: the two option orders must print different transcripts")
    check("Warning: DRF won't make semantic analysis\n" not in seen[0],
          "19.19: -no-semantic before -verbose prints no warning")
    check("Warning: DRF won't make semantic analysis\n" in seen[1],
          "19.19: -verbose before -no-semantic prints the warning")


def test_join_symbol_list_positional_reaches_the_ddb():
    """The dotless trailing positional is the additional-symbols list,
    exactly as in --to-json: IFDEFS.DSF's `#ifdef "CLIONLY"` block adds
    a SET 17, so the DDB bytes differ from the no-symbols run. Both are
    compared against the committed goldens/the reference flow."""
    ifdefs = IFDEFS_DSF_FIXTURE
    golden = JOIN_DDB_GOLDENS / "IFDEFS" / "NEXTDAAD_EN_v3_opt.ddb"
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp, fixture=ifdefs)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "bare.ddb", "-v3"],
                      cwd=str(tmpdir))
        check(rc == 0, f"symbols bare: expected exit 0, got {rc}\n{out}")
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "sym.ddb", "-v3",
                       "CLIONLY,SECOND"], cwd=str(tmpdir))
        check(rc == 0, f"symbols list: expected exit 0, got {rc}\n{out}")
        bare, sym = tmpdir / "bare.ddb", tmpdir / "sym.ddb"
        check(bare.exists() and sym.exists(),
              "symbols: expected both DDBs to be written")
        if not (bare.exists() and sym.exists()):
            return
        check(bare.read_bytes() != sym.read_bytes(),
              "symbols: the CLIONLY list must change the DDB bytes")
        if golden.exists():
            check(bare.read_bytes() == golden.read_bytes(),
                  "symbols: the no-list DDB must match the committed golden")
        if _flow_available():
            with tempfile.TemporaryDirectory() as reftmp:
                refdir = _join_dir(reftmp, fixture=ifdefs)
                ref_rc, _ = _flow_run(refdir,
                                      ("NEXTDAAD", "g.dsf", "-v3",
                                       "CLIONLY,SECOND"),
                                      ("NEXTDAAD", "EN", "g.json", "sym.ddb"))
                check(ref_rc == 0, f"symbols: reference flow exited {ref_rc}")
                check((refdir / "sym.ddb").read_bytes() == sym.read_bytes(),
                      "symbols: DDB bytes differ from the reference flow")


def test_join_dot_heuristic_claims_the_ddb_slot():
    """The 19.4 heuristic is --to-json's, verbatim: a dotted post-input
    argument claims the output slot - in the join that slot is the DDB
    name - and a dotless one is the symbol list, leaving the DDB to its
    drb-derived default. The join's own --json=<path> is the single
    carve-out (a dotted argument that stays an option), since letting
    the tee claim the DDB name would redirect the very compile it is
    meant to leave untouched."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "chosen.ddb"],
                      cwd=str(tmpdir))
        check(rc == 0, f"dot heuristic: expected exit 0, got {rc}\n{out}")
        check((tmpdir / "chosen.ddb").exists(),
              "dot heuristic: the dotted argument must name the DDB")
        check(not (tmpdir / "g.DDB").exists(),
              "dot heuristic: the default DDB name must not be used too")

        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "--json=tee.json"],
                      cwd=str(tmpdir))
        check(rc == 0, f"dot carve-out: expected exit 0, got {rc}\n{out}")
        check((tmpdir / "tee.json").exists(),
              "dot carve-out: --json=path must tee, not name the DDB")
        check((tmpdir / "g.DDB").exists(),
              "dot carve-out: the DDB keeps its drb-derived default name")

    # The carve-out only DECLINES the slot; it does not hand it to the
    # next argument. drf's heuristic looks at the FIRST post-input
    # argument only, so `--json=x out.ddb` leaves the slot unclaimed and
    # out.ddb reaches the options loop as the symbol list.
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "--json=tee.json",
                       "out.ddb", "-verbose"], cwd=str(tmpdir))
        check(rc == 0, f"dot carve-out order: expected exit 0, got {rc}\n{out}")
        check("Added Symbol: OUT.DDB=1\n" in out,
              f"dot carve-out order: out.ddb must become the symbol list, "
              f"got {out!r}")
        check((tmpdir / "g.DDB").exists() and
              not (tmpdir / "out.ddb").exists(),
              "dot carve-out order: the DDB keeps its default name")


# --- The drb option set routed through the join ------------------------
#
# MEASURED against the reference flow 2026-08-27, BLANK_EN as g.dsf,
# -v3 on the drf stage and the option on the drb stage:
#   -x          DDB 1940 bytes (the TX sections leave the DDB) + 0.XMB
#   -b=0x8000   DDB 2038 bytes, "Database starts at 32768 (0x8000)",
#               "Database ends at address 34806 (0x87F6)"
#   -c          NEXTDAAD refuses #classic by design, so this row runs on
#               ZX 48K: "Database starts at 33792 (0x8400)", and the
#               bytes differ from the same compile without -c
#   -b=0        "Target: NEXTDAAD" then
#               "Error: Invalid base address in -B=0.", exit 2
# Every row's DDB (and 0.XMB) is byte-compared to the flow's when a
# reference is configured; the pinned lines hold without one.

JOIN_DRB_OPTION_CASES = (
    ("-x", ("NEXTDAAD", "EN"), ("NEXTDAAD",), ("-x",),
     ("Total DDB size is 1940 bytes.\n",), ("0.XMB",)),
    ("-b=0x8000", ("NEXTDAAD", "EN"), ("NEXTDAAD",), ("-b=0x8000",),
     ("Database starts at 32768 (0x8000)\n",
      "Database ends at address 34806 (0x87F6)\n"), ()),
    ("-c", ("ZX", "48K", "EN"), ("ZX", "48K"), ("-c",),
     ("Database starts at 33792 (0x8400)\n",), ()),
)


def test_join_drb_option_set_reaches_the_drb_stage():
    """-x, -b= and -c routed by name to the drb stage: pinned stdout
    figures, the sidecar files each writes, and byte equality with the
    reference flow driven with the same option."""
    for (label, join_pos, drf_pos, opt, pins, extras) in JOIN_DRB_OPTION_CASES:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = _join_dir(tmp)
            rc, out = run([*join_pos, "g.dsf", "out.ddb", "-v3", *opt],
                          cwd=str(tmpdir))
            check(rc == 0, f"drb opt {label}: expected exit 0, got {rc}\n{out}")
            for pin in pins:
                check(pin in out,
                      f"drb opt {label}: expected {pin!r} in {out!r}")
            built = tmpdir / "out.ddb"
            check(built.exists(), f"drb opt {label}: expected out.ddb")
            if not built.exists():
                continue
            for extra in extras:
                check((tmpdir / extra).exists(),
                      f"drb opt {label}: expected {extra} to be written")
            if not _flow_available():
                continue
            with tempfile.TemporaryDirectory() as reftmp:
                refdir = _join_dir(reftmp)
                ref_rc, _ = _flow_run(refdir, (*drf_pos, "g.dsf", "-v3"),
                                      (*join_pos, "g.json", "out.ddb", *opt))
                check(ref_rc == 0,
                      f"drb opt {label}: reference flow exited {ref_rc}")
                check((refdir / "out.ddb").read_bytes() == built.read_bytes(),
                      f"drb opt {label}: DDB bytes differ from the flow")
                for extra in extras:
                    check((refdir / extra).read_bytes() ==
                          (tmpdir / extra).read_bytes(),
                          f"drb opt {label}: {extra} differs from the flow")


def test_join_classic_flag_changes_the_ddb():
    """Leg (d) for -c: the flagged and bare compiles must differ, or the
    row above would pass on a flag that never reached the back end."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        for name, opt in (("bare.ddb", ()), ("classic.ddb", ("-c",))):
            rc, out = run(["ZX", "48K", "EN", "g.dsf", name, "-v3", *opt],
                          cwd=str(tmpdir))
            check(rc == 0, f"-c leg d {name}: expected exit 0, got {rc}\n{out}")
        bare, classic = tmpdir / "bare.ddb", tmpdir / "classic.ddb"
        check(bare.exists() and classic.exists(),
              "-c leg d: expected both DDBs")
        if bare.exists() and classic.exists():
            check(bare.read_bytes() != classic.read_bytes(),
                  "-c leg d: -c must change the DDB bytes")


def test_join_bad_base_address_reports_the_first_one():
    """A -b= out of bounds is deferred to the drb stage's own position
    (after the Target line). drb's loop BREAKS at its first option error,
    so with two bad options the FIRST is reported - the join must match
    --from-json, which is the ported stage."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = _join_dir(tmp)
        tail = ("Target: NEXTDAAD\n"
                "Error: Invalid base address in -B=0.\n")
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-b=0"], cwd=str(tmpdir))
        check(rc == 2, f"-b=0: expected exit 2, got {rc}\n{out}")
        check(out.endswith(tail), f"-b=0: unexpected stdout {out!r}")
        check(not (tmpdir / "g.DDB").exists(),
              "-b=0: no DDB on an option error")

        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-b=0", "-b=70000"],
                      cwd=str(tmpdir))
        check(rc == 2, f"-b= first-wins: expected exit 2, got {rc}\n{out}")
        check(out.endswith(tail),
              f"-b= first-wins: the FIRST bad option must be reported, "
              f"got {out!r}")

        rc, out = run(["--from-json", "NEXTDAAD", "EN",
                       str(_fixture_copy(tmpdir)), "o.ddb",
                       "-b=0", "-b=70000"])
        check(rc == 2, f"-b= first-wins: --from-json exited {rc}\n{out}")
        check(out.endswith("Error: Invalid base address in -B=0.\n"),
              f"-b= first-wins: --from-json must agree, got {out!r}")


# --------------------------------------------------------------------
# X-condacts through the JOIN. The 2b cases
# above run --to-json only, so nothing pinned what the join does with a
# fixture the guarded block acts on - and the join reaches a refusal
# --to-json cannot see at all: the DRB stage's own XPICTURE rejection.
#
# MEASURED against the reference flow on 2026-08-27, same spliced
# BLANK_EN + `XPICTURE 0` source (_xpicture_dsf):
#   bare                  drf compiles clean, then the DRB stage refuses
#                         with "Error: XPICTURE condact has been
#                         deprecated.." at its own transcript position,
#                         exit 2. The reference leaves a PARTIAL g.DDB
#                         (drb.php writes incrementally); ndrc writes
#                         none - the join leg's own accepted directional
#                         carve-out, so only ndrc's side is asserted on.
#   -replace-xcondacts    the drf stage refuses first (exit 1, the same
#                         line the 2b case pins), so the drb stage never
#                         runs and the transcript stops there.
# --------------------------------------------------------------------

JOIN_XPICTURE_DRB_REFUSAL = (
    "Generating DAAD V3 DDB\n"
    "Reading g.dsf\n"
    "Checking Syntax...\n"
    "Updating forward references...\n"
    "Generating g.json [Classic mode OFF]\n"
    "g.json generated.\n"
    "Target: NEXTDAAD\n"
    "Error: XPICTURE condact has been deprecated..\n"
)


def test_join_xpicture_bare_dies_at_the_drb_stage():
    """The drf stage accepts XPICTURE and the drb stage rejects it, so
    the whole drf transcript precedes the error - a refusal the 2b
    --to-json cases cannot reach."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _xpicture_dsf(tmpdir)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-v3"], cwd=str(tmpdir))
        check(rc == 2, f"join xpicture: expected exit 2, got {rc}\n{out}")
        check(out == JOIN_XPICTURE_DRB_REFUSAL,
              f"join xpicture: unexpected stdout {out!r}")
        check(not (tmpdir / "g.DDB").exists(),
              "join xpicture: ndrc must write no DDB on a drb-stage refusal")
        if _flow_available():
            with tempfile.TemporaryDirectory() as reftmp:
                refdir = Path(reftmp)
                _xpicture_dsf(refdir)
                ref_rc, ref_out = _flow_run(refdir, ("NEXTDAAD", "g.dsf", "-v3"),
                                            ("NEXTDAAD", "EN", "g.json"))
            check(ref_rc == rc,
                  f"join xpicture: ndrc exit {rc} but flow exits {ref_rc}")
            check(ref_out == out,
                  f"join xpicture: transcript {out!r} != flow {ref_out!r}")


def test_join_xpicture_replace_xcondacts_stops_at_the_drf_stage():
    """-replace-xcondacts routed to the drf stage through the join: the
    same rejection the 2b --to-json case pins, now aborting the flow
    before the drb stage prints anything."""
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        _xpicture_dsf(tmpdir)
        rc, out = run(["NEXTDAAD", "EN", "g.dsf", "-v3",
                       "-replace-xcondacts"], cwd=str(tmpdir))
        check(rc == 1,
              f"join xpicture -replace-xcondacts: expected exit 1, got {rc}\n"
              f"{out}")
        check(out == ("Generating DAAD V3 DDB\n"
                      "Reading g.dsf\n"
                      "Checking Syntax...\n"
                      + XPICTURE_ERROR_LAST_LINE),
              f"join xpicture -replace-xcondacts: unexpected stdout {out!r}")
        check(not (tmpdir / "g.DDB").exists(),
              "join xpicture -replace-xcondacts: no DDB on a drf-stage "
              "rejection")
        if _flow_available():
            with tempfile.TemporaryDirectory() as reftmp:
                refdir = Path(reftmp)
                _xpicture_dsf(refdir)
                ref_rc, ref_out = _flow_run(
                    refdir, ("NEXTDAAD", "g.dsf", "-v3", "-replace-xcondacts"),
                    ("NEXTDAAD", "EN", "g.json"))
            check(ref_rc == rc,
                  f"join xpicture -replace-xcondacts: ndrc exit {rc} but flow "
                  f"exits {ref_rc}")
            check(ref_out == out,
                  f"join xpicture -replace-xcondacts: transcript {out!r} != "
                  f"flow {ref_out!r}")


def main():
    check(FIXTURE.exists(), f"fixture missing: {FIXTURE}")
    check(DSF_FIXTURE.exists(), f"fixture missing: {DSF_FIXTURE}")
    check(IFDEFS_DSF_FIXTURE.exists(),
          f"fixture missing: {IFDEFS_DSF_FIXTURE}")
    check(NDRC.exists(), f"ndrc binary not found: {NDRC}")
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print(f"FAIL: {f}")
        print(f"test_cli: {len(FAILURES)} failures")
        return 1
    print("test_cli: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
