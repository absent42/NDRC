# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Drives the reference DRC toolchain: drf.exe then php drb.php.

Two behaviours of the reference are worked around here rather than by
patching the fork, so that goldens stay generatable from an unmodified
checkout:

  DEF-3  drb.php:1240 carries an uncommented debug echo that prints
         'Debug: Checking subtarget ...' for every subtargeted run. It
         pollutes stdout but never the DDB, so stdout is captured and
         reported, never parsed for correctness.

  Clock symbols  drf.pas:259-262 injects YEARHIGH, YEARLOW, MONTH and DAY
         from the system clock. They appear in the JSON, so JSON
         comparisons must canonicalise them away. They reach the DDB only
         if a source references them, which no committed fixture does.

Each run gets a uniquely-named directory under workdir (via
tempfile.mkdtemp), because both stages write fixed-name files into the
current directory - g.json, g.DDB and DRF's g.___ scratch file - so
concurrent runs cannot collide. It is removed on success and kept on
failure for inspection.

drb.php writes 0.XMB in TWO places. generateXMessages (drb.php:449,
called at drb.php:1920-1923) runs whenever the JSON's xmessages array
is non-empty - CONDACTS has two XMESSAGEs - and unconditionally writes
0.XMB into the working directory, besides computing the xMessageOffsets
the XMES condact rewrite bakes into the DDB. Separately, -X sets
dumpToXMB (drb.php:1376, guarded at drb.php:1926) to dump ordinary TX
sections there too; the oracle never passes -X. So a 0.XMB DOES appear
in CONDACTS run directories; the goldens are the DDB alone, and the run
directory is discarded on success. An earlier revision of this comment
claimed no XMB was ever produced - that was wrong.

stage_run_dir() is the common per-run setup both run_reference and
run_reference_from_json call: it copies the DSF to g.DSF, copies a
sidecar tests/fixtures/<FIXTURE>.tok to g.tok when one exists, and
asserts the fresh run dir carries no stray *.XMB before either stage
runs (task-2-brief.md Interfaces). run_reference deliberately still does
not collect *.XMB on success - see the CONDACTS paragraph above: doing
so would flip CONDACTS's four PLAIN committed goldens from single-file
to multi-file and break manifest compatibility. run_reference_from_json
collects them into xmb_files as before, for the --from-json sweep's own
comparison (fresh-vs-fresh, no committed golden involved) - and, from
task-4-brief.md on, is also the routing gen_goldens.py's curated_jobs()
uses for its own flagged (-x) jobs, whose committed goldens are
DELIBERATELY multi-file (CARRY 1): a flagged job opts into XMB
collection by construction (its own combo.flags), so reusing this
function for goldens generation carries none of run_reference's
CONDACTS risk.
"""
from __future__ import annotations

import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .config import OracleConfig
from .matrix import Combo

TIMEOUT_SECONDS = 120


@dataclass
class RefResult:
    ok: bool
    ddb: bytes | None
    json_text: str | None
    stdout: str
    stderr: str
    stage: str          # "drf", "drb", or "done"


@dataclass
class FromJsonRefResult:
    """Result of run_reference_from_json.

    run_reference() is not enough for the --from-json gate: it runs DRB
    non-verbose (no map/token lines to compare against ndrc -v), and its
    json_text has been through a latin-1 text read, which is lossy for a
    byte-for-byte comparison against what ndrc --from-json is handed (the
    JSON's CRLF line endings matter). json_bytes here is the untouched
    binary read of the same g.json DRB then consumed, so both sides of
    the --from-json comparison start from identical bytes.

    stdout is the combined DRF+DRB transcript (DRF's own diagnostics are
    part of it, same as run_reference), kept for failure reporting.
    drb_stdout is DRB's stdout ALONE - what the --from-json sweep compares
    byte-for-byte against ndrc's own stdout (minus ndrc's banner line),
    since ndrc --from-json only ever runs the DRB half of the pipeline.
    It is only ever non-empty once DRB has actually run (stage "drb" or
    "done"); "" on a drf-stage failure, same as ddb is None there.

    xmb_files is every *.XMB file's name and raw bytes, collected from the
    run directory right before it is removed on success (module docstring:
    drb.php can write 0.XMB alongside the DDB). Empty on any failure path,
    same reasoning as ddb being None there - a failed run's directory is
    left on disk for inspection instead (see run_reference's docstring).

    jddb_files is the same idea for *.jddb/*.JDDB files (task-7-brief.md
    Step 5): generateJDDB (drb.php:1399-1445) writes one whenever the
    target is HTML, lowercased and with '.ddb' replaced by '.jddb' in the
    output filename (drb.php:1402-1403) - collected the same way, right
    before the run directory is removed on success.
    """
    ok: bool
    json_bytes: bytes | None
    ddb: bytes | None
    stdout: str
    drb_stdout: str
    stderr: str
    stage: str          # "drf", "drb", or "done"
    xmb_files: dict[str, bytes]
    jddb_files: dict[str, bytes]


def stage_extern_assets(run_dir: Path, dsf: Path) -> None:
    """Copies every EXT_*.BIN companion beside dsf into run_dir, when
    dsf is EXTERNS.DSF (task-3 controller ruling: "stage ALL files
    matching tests/fixtures/EXT_*.BIN into the run dir whenever the
    fixture is EXTERNS"). DRB (and ndrc) resolve extern paths against
    their own cwd (generateExterns, drb.php:114-115's relative
    file_exists/fopen - no path handling), so the three EXT_MAIN.BIN/
    EXT_SFX.BIN/EXT_INT.BIN fixture assets have to land in the run
    directory itself, not just beside the DSF.

    Chosen mechanism (of the two the ruling allowed): a glob keyed off
    the fixture name, not a hand-maintained per-fixture asset-name list -
    a second EXT_*.BIN added to the EXTERNS fixture later is picked up
    here with no matching edit required. dsf.parent is tests/fixtures/
    for every real caller (run_reference/run_reference_from_json always
    pass a path built from FIXTURES), so this needs no separate
    "fixtures directory" parameter of its own.
    """
    if dsf.stem != "EXTERNS":
        return
    for asset in sorted(dsf.parent.glob("EXT_*.BIN")):
        shutil.copyfile(asset, run_dir / asset.name)


def stage_include_assets(run_dir: Path, dsf: Path) -> None:
    """Copies the INCLUDE fixture's two sidecars into run_dir, when dsf
    is INCLUDE.DSF: INCLUDE2.DSF (the #include target
    - Preparse (drf.pas:154-208) resolves the include filename with no
    path handling, same relative-to-cwd lookup stage_extern_assets's own
    docstring describes for externs) and INC_DATA.BIN (the #incbin
    target, looked up the same way by ParseProcessCondacts,
    USintactic.pas:794-808). Both are sidecars despite INCLUDE2.DSF's
    .DSF extension - neither is ever compiled standalone (absent from
    gen_goldens.py's curated_jobs()), so a name-keyed hook here, not a
    glob, matches how the fixture actually needs to be resolved: exactly
    these two names, nothing else.
    """
    if dsf.stem != "INCLUDE":
        return
    for name in ("INCLUDE2.DSF", "INC_DATA.BIN"):
        asset = dsf.parent / name
        if asset.exists():
            shutil.copyfile(asset, run_dir / name)


def stage_run_dir(run_dir: Path, dsf: Path) -> None:
    """Common per-run staging, shared by run_reference and
    run_reference_from_json (task-2-brief.md Interfaces: "the shared
    run-dir setup used by BOTH gen_goldens and the sweep").

    Copies dsf to g.DSF, then the sidecar hook: when tests/fixtures/
    <FIXTURE>.tok exists beside the DSF, copies it in too as g.tok (the
    name DRB derives from g.json - drb.php:1749-1756), before DRF/DRB
    ever runs. Then stage_extern_assets (task-3): the EXTERNS fixture's
    own EXT_*.BIN companions, copied in the same way for the same
    relative-path reason. Then stage_include_assets (task-5): the
    INCLUDE fixture's INCLUDE2.DSF/INC_DATA.BIN, same reasoning.

    Then the stale-XMB guard: run_dir is always a directory this run's
    own tempfile.mkdtemp just created, so no *.XMB can genuinely be
    found here right after staging - the assert exists to PIN that
    guarantee (spec section 2: no stale XMB survives into a run) as a
    hard failure rather than an unstated invariant, so a future change
    that stops using a fresh directory per run trips it immediately
    instead of silently reusing another run's XMB output.
    """
    shutil.copyfile(dsf, run_dir / "g.DSF")
    sidecar = dsf.with_suffix(".tok")
    if sidecar.exists():
        shutil.copyfile(sidecar, run_dir / "g.tok")
    stage_extern_assets(run_dir, dsf)
    stage_include_assets(run_dir, dsf)
    stale = sorted(p.name for p in run_dir.glob("*.XMB"))
    assert not stale, (
        f"stale *.XMB found in freshly staged run dir {run_dir}: {stale} "
        f"(spec section 2 guarantee violated)")


def _run(args: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        args,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        errors="replace",
        timeout=TIMEOUT_SECONDS,
    )


def run_reference(cfg: OracleConfig, dsf: Path, combo: Combo) -> RefResult:
    """Compiles one DSF for one combo. Returns bytes, never writes goldens."""
    run_dir = Path(tempfile.mkdtemp(
        prefix=f"ref_{dsf.stem}_{combo.slug}_", dir=str(cfg.workdir)))

    stage_run_dir(run_dir, dsf)

    drf_args = [str(cfg.drf), *combo.drf_args, "g.DSF", "g.json"]
    if combo.v3:
        drf_args.append("-v3")
    # Failure paths deliberately leave run_dir in place - the failing
    # g.json/g.DDB and captured output are exactly what's needed to
    # diagnose the failure.
    try:
        p1 = _run(drf_args, run_dir)
    except subprocess.TimeoutExpired:
        return RefResult(False, None, None, "",
                         f"DRF timed out after {TIMEOUT_SECONDS} seconds", "drf")
    if p1.returncode != 0:
        return RefResult(False, None, None, p1.stdout, p1.stderr, "drf")

    json_path = run_dir / "g.json"
    json_text = json_path.read_text(encoding="latin-1") if json_path.exists() else None

    drb_args = [str(cfg.php), str(cfg.drb), *combo.drb_args, "g.json", "g.DDB"]
    if combo.classic:
        drb_args.append("-c")
    drb_args.extend(combo.flags)
    try:
        p2 = _run(drb_args, run_dir)
    except subprocess.TimeoutExpired:
        return RefResult(False, None, json_text, p1.stdout,
                         p1.stderr + f"\nDRB timed out after {TIMEOUT_SECONDS} seconds",
                         "drb")
    if p2.returncode != 0:
        return RefResult(False, None, json_text,
                         p1.stdout + p2.stdout, p1.stderr + p2.stderr, "drb")

    ddb_path = run_dir / "g.DDB"
    if not ddb_path.exists():
        return RefResult(False, None, json_text,
                         p1.stdout + p2.stdout,
                         p1.stderr + p2.stderr + "\nDRB produced no DDB",
                         "drb")

    ddb = ddb_path.read_bytes()
    shutil.rmtree(run_dir, ignore_errors=True)
    return RefResult(True, ddb, json_text,
                     p1.stdout + p2.stdout, p1.stderr + p2.stderr, "done")


def collect_xmb_files(run_dir: Path) -> dict[str, bytes]:
    """Every *.XMB file's name and raw bytes, read from run_dir.

    Called right before the run directory is removed on success. drb.php
    writes 0.XMB into the working directory whenever the JSON's xmessages
    array is non-empty (module docstring above); globbing rather than
    hard-coding "0.XMB" covers any other *.XMB drb.php may produce without
    this needing to track drb.php's naming beyond the extension.
    """
    return {p.name: p.read_bytes() for p in sorted(run_dir.glob("*.XMB"))}


def collect_jddb_files(run_dir: Path) -> dict[str, bytes]:
    """Every *.jddb/*.JDDB file's name and raw bytes, read from run_dir.

    Same mechanism as collect_xmb_files: called right before the run
    directory is removed on success. generateJDDB (drb.php:1399-1445)
    lowercases the output filename before writing, so a real run always
    produces a lowercase ".jddb" name, but the glob covers both cases
    rather than hard-coding that assumption.
    """
    return {p.name: p.read_bytes()
            for p in sorted(run_dir.glob("*.[jJ][dD][dD][bB]"))}


@dataclass
class DrfOnlyResult:
    """Result of run_drf_only - DRF alone, no DRB stage.

    stage is "drf" (DRF itself failed or wrote no JSON) or "done".
    json_bytes is the untouched binary read of g.json, same rationale
    as FromJsonRefResult.json_bytes: a byte-for-byte JSON comparison
    (task-10's --to-json gate) must not go through any lossy text
    decode first.
    """
    ok: bool
    json_bytes: bytes | None
    stdout: str
    stderr: str
    returncode: int
    stage: str          # "drf" or "done"


def run_drf_only(cfg: OracleConfig, dsf: Path, combo: Combo,
                 extra_args: tuple[str, ...] = ()) -> DrfOnlyResult:
    """Runs DRF alone - no DRB stage - for the --to-json gate (task-10,
    spec section 6), which compares DRF's own JSON output directly
    against ndrc --to-json; no DDB/DRB half is involved.

    Mirrors run_reference's own DRF invocation exactly (stage_run_dir,
    combo.drf_args, the -v3 flag), with extra_args (e.g. "-verbose")
    appended verbatim after it - same position --to-json's own CLI
    shape expects options in (drf.pas:350-413's options loop, ported at
    main.c's run_to_json).
    """
    run_dir = Path(tempfile.mkdtemp(
        prefix=f"drfonly_{dsf.stem}_{combo.slug}_", dir=str(cfg.workdir)))

    stage_run_dir(run_dir, dsf)

    drf_args = [str(cfg.drf), *combo.drf_args, "g.DSF", "g.json"]
    if combo.v3:
        drf_args.append("-v3")
    drf_args.extend(extra_args)
    try:
        p = _run(drf_args, run_dir)
    except subprocess.TimeoutExpired:
        return DrfOnlyResult(False, None, "",
                             f"DRF timed out after {TIMEOUT_SECONDS} seconds",
                             -1, "drf")
    if p.returncode != 0:
        return DrfOnlyResult(False, None, p.stdout, p.stderr, p.returncode, "drf")

    json_path = run_dir / "g.json"
    if not json_path.exists():
        return DrfOnlyResult(False, None, p.stdout,
                             p.stderr + "\nDRF produced no JSON",
                             p.returncode, "drf")

    json_bytes = json_path.read_bytes()
    shutil.rmtree(run_dir, ignore_errors=True)
    return DrfOnlyResult(True, json_bytes, p.stdout, p.stderr, p.returncode, "done")


def run_reference_from_json(cfg: OracleConfig, dsf: Path,
                            combo: Combo) -> FromJsonRefResult:
    """Compiles one DSF for one combo, verbose, returning raw JSON bytes too.

    For the verify.py --from-json gate: the fresh JSON bytes are the input
    fed to ndrc --from-json, and DRB's OWN -v stdout (drb_stdout, kept
    apart from DRF's) is what ndrc's stdout is compared against, so both
    need to survive past this function unlike in run_reference. The run
    directory's *.XMB files are collected the same way. Otherwise
    identical to run_reference in staging, cleanup and error-path
    conventions.
    """
    run_dir = Path(tempfile.mkdtemp(
        prefix=f"fromjson_{dsf.stem}_{combo.slug}_", dir=str(cfg.workdir)))

    stage_run_dir(run_dir, dsf)

    drf_args = [str(cfg.drf), *combo.drf_args, "g.DSF", "g.json"]
    if combo.v3:
        drf_args.append("-v3")
    try:
        p1 = _run(drf_args, run_dir)
    except subprocess.TimeoutExpired:
        return FromJsonRefResult(False, None, None, "", "",
                                 f"DRF timed out after {TIMEOUT_SECONDS} seconds",
                                 "drf", {}, {})
    if p1.returncode != 0:
        return FromJsonRefResult(False, None, None, p1.stdout, "",
                                 p1.stderr, "drf", {}, {})

    json_path = run_dir / "g.json"
    if not json_path.exists():
        return FromJsonRefResult(False, None, None, p1.stdout, "",
                                 p1.stderr + "\nDRF produced no JSON", "drf", {}, {})
    json_bytes = json_path.read_bytes()

    drb_args = [str(cfg.php), str(cfg.drb), *combo.drb_args,
               "g.json", "g.DDB", "-v"]
    if combo.classic:
        drb_args.append("-c")
    drb_args.extend(combo.flags)
    try:
        p2 = _run(drb_args, run_dir)
    except subprocess.TimeoutExpired:
        return FromJsonRefResult(False, json_bytes, None, p1.stdout, "",
                                 p1.stderr + f"\nDRB timed out after {TIMEOUT_SECONDS} seconds",
                                 "drb", {}, {})
    if p2.returncode != 0:
        return FromJsonRefResult(False, json_bytes, None,
                                 p1.stdout + p2.stdout, p2.stdout,
                                 p1.stderr + p2.stderr, "drb", {}, {})

    ddb_path = run_dir / "g.DDB"
    if not ddb_path.exists():
        return FromJsonRefResult(False, json_bytes, None,
                                 p1.stdout + p2.stdout, p2.stdout,
                                 p1.stderr + p2.stderr + "\nDRB produced no DDB",
                                 "drb", {}, {})

    ddb = ddb_path.read_bytes()
    xmb_files = collect_xmb_files(run_dir)
    jddb_files = collect_jddb_files(run_dir)
    shutil.rmtree(run_dir, ignore_errors=True)
    return FromJsonRefResult(True, json_bytes, ddb,
                             p1.stdout + p2.stdout, p2.stdout,
                             p1.stderr + p2.stderr, "done", xmb_files, jddb_files)
