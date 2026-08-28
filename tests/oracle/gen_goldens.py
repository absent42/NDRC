# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Generates or verifies the committed golden DDBs.

  python gen_goldens.py           regenerate every golden
  python gen_goldens.py --check   verify goldens still match the reference

Both modes need the reference DRC toolchain: --check re-derives every
golden from the reference and compares, which is the whole point of it -
it answers "are these goldens still what the reference produces today".

It is NOT the CI gate. CI needs a check that runs with no PHP and no
FreePascal, and that is verify.py --self-check, which compares the
committed goldens against their manifest and needs nothing but this
repository.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from ndrcoracle.config import load_config, ConfigError
from ndrcoracle.matrix import TARGETS, LANGUAGES, Combo, all_combos
from ndrcoracle.reference import run_reference, run_reference_from_json

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "fixtures"
GOLDENS = ROOT / "goldens"
MANIFEST = GOLDENS / "manifest.json"

FIXTURE_NAMES = ["BLANK_EN", "BLANK_ES", "STARTER", "CONDACTS", "BIGDDB"]


def curated_jobs():
    """(fixture, combo) pairs that get a committed golden.

    Three sets, each answering a different coverage question:

      Set A: every target/subtarget pair once, on BLANK_EN, at EN/v3/opt,
      to catch a per-target regression in the common case.

      Set B: classic mode swept across every target EXCEPT NEXTDAAD, on
      BLANK_EN, at EN/v3. NDRC hard-errors on #classic for the NEXTDAAD
      target: a classic database exists to be read by the original
      pre-DRC interpreters, and those cannot read a NEXTDAAD database at
      all, so a NEXTDAAD/classic golden is one NDRC will never be able to
      produce. Every other target accepts #classic and ships it, so that
      is where classic coverage belongs. The omission of NEXTDAAD here is
      deliberate, not an oversight.

      Set C: NEXTDAAD, the target that ships, on every fixture, across
      {EN, ES} x {v2, v3}, optimised only. Classic is dropped here for
      the same reason Set B adds it everywhere else.

      Set D: v2 insurance on two non-NEXTDAAD pairs (ST and ZX/48K),
      BLANK_EN, EN, opt. In drb.php the version axis and the target
      axis touch disjoint code - every $v3code consultation site
      (drb.php:841, 879, 956, 998, 1817, 1839) ignores $target, and no
      target-dependent site reads $v3code - so v2 on NEXTDAAD plus v3
      per target should cover the product. These two goldens exist as
      insurance against that code-reading argument being wrong, on the
      pairs whose layout differs most from NEXTDAAD: ST (big-endian,
      padded) and ZX/48K (base 0x8400). Owner-ordered 2026-08-25.

      Set E: embedded externs (task-3-brief.md Step 5), on the EXTERNS
      fixture, plain combos (no flags), EN/v3/opt, across three targets
      chosen for three different address-arithmetic regimes generateExterns'
      currentAddress bump (drb.php:127) has to survive: NEXTDAAD (base 0,
      no padding - the plain case), ZX/48K (non-zero base 0x8400, so each
      entry's extvec[] value and its "loaded at" echo are ABSOLUTE
      addresses, not file offsets), and ST (big-endian words plus a
      padding platform, so a padding byte can land inside - or, per
      addPaddingIfRequired's drb.php:1937 call AFTER generateExterns
      returns, immediately after - the externs region). Together with Set
      A/D's existing non-NEXTDAAD coverage these three pairs are the
      minimum needed to see the externs code path under every one of
      those three regimes at least once.

      Set F: the -X TX-dump (task-4-brief.md Step 4), NEXTDAAD/EN/v3/opt,
      flags=("-x",), on two fixtures: BLANK_EN (the plain dump path - no
      XMessages, so the -X append open at drb.php:1888-1894 finds no
      0.XMB and starts a fresh one) and CONDACTS (CONDACTS's own two
      XMESSAGE condacts make generateXMessages write 0.XMB FIRST, so the
      -X open then appends to it from the existing-size cursor - the
      XMessages-then-append interplay reference.py's module docstring
      already describes for the plain, XMB-blind corpus path). These are
      the phase's first MULTI-FILE goldens - manifest_entry's "files"
      shape, each file verified independently by self_check/entry_files.

      Set G: XPLAY/mmlToBeep (task-5-brief.md Step 4), on the XPLAY
      fixture, plain combos (no flags), EN/v3/opt, across four targets
      chosen for three different duration/pitch pairs plus one swap:
      NEXTDAAD (base 100, pitch -24, beep_swap on - the target that
      ships), C64 (base 120, pitch -12), CPC (base 300, pitch 0 - the
      target whose base is large enough for the R-arm/N-arm double-
      adjustment defect to trip the 255 duration cap on this fixture's
      MML, task-5-report.md) and ZX/48K (base 195, pitch -24, beep_swap
      on - a second swap pair at a different base/duration regime than
      NEXTDAAD's). Together these exercise every BEEP/PAUSE rewrite this
      task ports at least once: the clamp, the ZX-family swap, both
      duration-adjustment defects, and the 255 cap.

      Set H: DEBUG condact hashing/emission (task-6-brief.md Step 4/5),
      on the DEBUG fixture, flags=("-d",), EN/v3/opt, across the three
      debug-allowed targets (NEXTDAAD, ZX/48K, CPC - t->debug_allowed)
      plus ST (the one drb.php:1802 retargets: !=ZX/CPC/NEXTDAAD clears
      debugMode). DEBUG.DSF's own `#debug` directive already sets
      debug_mode=1 in the JSON, so -d here is REDUNDANT on all four -
      COVERAGE ARGUMENT (owner): the flags_d_st stdout extra (verify.py)
      proves -d itself sets forced_debug/debugMode (on a fixture with no
      #debug directive of its own, BLANK_EN); these four goldens prove
      the variable, once set, drives both hashing (getCondactsHash,
      drb.php:776) and emission (drb.php:1116) correctly. Together, with
      no -d-only golden needed, that is compositionally sound coverage
      of -d end to end: one flag-effect proof plus four downstream-
      consequence proofs cover the same ground a fifth "-d changes this
      DDB" golden would, without needing one. Reference bytes staged and
      inspected before porting the C side (task-6-report.md): on
      NEXTDAAD/ZX/CPC (debug_mode true) DEBUG participates in the hash,
      so the fixture's twin-tail entries do NOT dedup-share; on ST
      (debug_mode retargeted false) DEBUG is skipped from both hash and
      emission, so the same two entries DO dedup-share into one address.

      Set I: -p (task-6-brief.md Step 5), BLANK_EN, NEXTDAAD/EN/v3/opt,
      flags=("-p",). NEXTDAAD is not a padding platform, so -p's own
      OR-term (`t->padding_platform || forced_padding`) is the only
      thing under test here; BLANK_EN's layout has no odd-address
      padding decision point on NEXTDAAD normally; the golden exists so
      a regression in that OR-term (e.g. losing forced_padding entirely)
      shows up as a byte diff rather than passing silently. --only
      _opt_p (Gates) selects this pair via its slug's flag suffix.

      Set J: the .tok override (task-7-brief.md), TOKFILE fixture (a
      byte-identical copy of BLANK_EN, distinct name so the sidecar hook
      keys cleanly), NEXTDAAD/EN/v3/opt, plain combo (no flags - the
      override is picked up purely from tests/fixtures/TOKFILE.tok
      existing beside the DSF, staged as g.tok by stage_run_dir/
      _stage_ndrc_run_dir's shared sidecar hook, task-2-brief.md). The
      golden exists so a regression in tokens_load_override's lookup or
      in the "basic" compressable-set arm (locations only, drb.php:315)
      shows up as a byte diff: TOKFILE.tok's 4 hand-picked tokens (two
      phrases from BLANK_EN's own location texts, one location-only
      literal, one absent string) are measured against a live reference
      run (task-7-report.md) to give one surviving token (occurs twice
      across the two non-empty locations), one "waste 1 byte" drop and
      one "not used" drop - so the golden also pins the two-pass
      compressor's savings arithmetic on a real (not synthetic) fixture.
      A plain combo routes this through run_reference, same as Sets A-E;
      the -v transcript this depends on (the "Loading tokens from
      g.tok." line) is exercised separately, by the --from-json sweep
      (run_reference_from_json always passes -v) rather than here - see
      task-7-report.md for the confirmation that gen_goldens' own
      generation path does NOT run DRB verbosely.

      Set K: the corpus-join fixtures, NEXTDAAD/EN/v3/opt only (not the
      full EN/ES x v2/v3 sweep Set C gives the FIXTURE_NAMES fixtures),
      on EXPR, IFDEFS and INCLUDE. XMSG is deliberately excluded from
      this set: its `XDATA "xdata payload"` condact is valid input for
      DRF's own --to-json JSON, but the XDATA_OPCODE backend rewrite
      (drb.php:958-997, ported at emit_proc.c:268) requires the
      referenced other_strings text to be comma-separated numeric flag
      data ("baseflag,val1,val2,...") to build a DDB at all - "xdata
      payload" has no comma, so sizeof($dataArray)<2 and DRB refuses it
      outright. MEASURED live 2026-08-27: reference drb.php and ndrc
      --from-json both exit non-zero with the byte-identical "Error:
      There is not data enough in XDATA condact." - a shared, correctly-
      ported refusal, not an ndrc defect - so no DDB golden can be
      generated for XMSG by this recipe without first amending the
      fixture's own XDATA payload, which is outside this set's scope.

    Built from the matrix's own TARGETS/LANGUAGES rather than a
    hand-written target list, so the sweep in Set B stays correct on its
    own if a target is ever added or removed.
    """
    jobs = []
    seen = set()

    def add(fixture: str, combo) -> None:
        key = (fixture, combo.slug)
        if key not in seen:
            seen.add(key)
            jobs.append((fixture, combo))

    # Set A: every target/subtarget pair, BLANK_EN, EN/v3/opt.
    for combo in all_combos(languages=("EN",), versions=(True,), modes=(False,)):
        add("BLANK_EN", combo)

    # Set B: classic, every target except NEXTDAAD, BLANK_EN, EN/v3.
    non_nextdaad = {t: subs for t, subs in TARGETS.items() if t != "NEXTDAAD"}
    for combo in all_combos(targets=non_nextdaad, languages=("EN",),
                             versions=(True,), modes=(True,)):
        add("BLANK_EN", combo)

    # Set C: NEXTDAAD, every fixture, EN/ES x v2/v3, opt only.
    for name in FIXTURE_NAMES:
        for combo in all_combos(targets={"NEXTDAAD": [None]},
                                 languages=LANGUAGES, versions=(False, True),
                                 modes=(False,)):
            add(name, combo)

    # Set D: v2 insurance pairs, BLANK_EN, EN, opt.
    for target, sub in (("ST", None), ("ZX", "48K")):
        add("BLANK_EN", Combo(target, sub, "EN", False, False))

    # Set E: embedded externs, EXTERNS fixture, EN/v3/opt, three targets
    # (see the docstring above for why these three).
    for target, sub in (("NEXTDAAD", None), ("ZX", "48K"), ("ST", None)):
        add("EXTERNS", Combo(target, sub, "EN", True, False))

    # Set F: -X TX-dump, NEXTDAAD/EN/v3/opt, flags=("-x",), two fixtures
    # (see the docstring above for the plain-dump vs. XMessages-then-
    # append rationale). CARRY 1 (task-4-brief.md): these are the first
    # curated_jobs() entries with a non-empty combo.flags, which is what
    # routes them through the XMB-collecting runner below rather than
    # plain run_reference.
    add("BLANK_EN", Combo("NEXTDAAD", None, "EN", True, False, flags=("-x",)))
    add("CONDACTS", Combo("NEXTDAAD", None, "EN", True, False, flags=("-x",)))

    # Set G: XPLAY/mmlToBeep, XPLAY fixture, EN/v3/opt, four targets
    # (see the docstring above for why these four).
    for target, sub in (("NEXTDAAD", None), ("C64", None), ("CPC", None), ("ZX", "48K")):
        add("XPLAY", Combo(target, sub, "EN", True, False))

    # Set H: DEBUG condact hashing/emission, DEBUG fixture, flags=("-d",),
    # EN/v3/opt, the three debug-allowed targets plus ST's retarget (see
    # the docstring above for the compositional-coverage argument).
    for target, sub in (("NEXTDAAD", None), ("ZX", "48K"), ("CPC", None), ("ST", None)):
        add("DEBUG", Combo(target, sub, "EN", True, False, flags=("-d",)))

    # Set I: -p, BLANK_EN, NEXTDAAD/EN/v3/opt, flags=("-p",) (see the
    # docstring above).
    add("BLANK_EN", Combo("NEXTDAAD", None, "EN", True, False, flags=("-p",)))

    # Set J: the .tok override, TOKFILE fixture, NEXTDAAD/EN/v3/opt,
    # plain combo (see the docstring above).
    add("TOKFILE", Combo("NEXTDAAD", None, "EN", True, False))

    # Set K: the corpus-join fixtures, NEXTDAAD/EN/v3/opt only, EXPR/
    # IFDEFS/INCLUDE (see the docstring above for why XMSG is excluded).
    for name in ("EXPR", "IFDEFS", "INCLUDE"):
        add(name, Combo("NEXTDAAD", None, "EN", True, False))

    return jobs


def golden_file_path(fixture: str, slug: str, name: str, *,
                     root: Path = GOLDENS) -> Path:
    """tests/goldens/<FIXTURE>/<slug>.<name> (task-2-brief.md Interfaces).
    name is "ddb" for the primary database, or an XMB filename (e.g.
    "0.XMB") for a companion a run produced alongside it - e.g.
    BLANK_EN/NEXTDAAD_EN_v3_opt_x.0.XMB. root is overridable so tests can
    round-trip through a private directory instead of tests/goldens/.
    """
    return root / fixture / f"{slug}.{name}"


def golden_path(fixture: str, combo) -> Path:
    return golden_file_path(fixture, combo.slug, "ddb")


def manifest_entry(ddb: bytes, xmb_files: dict[str, bytes]) -> dict:
    """One manifest.json value (task-2-brief.md Interfaces): the existing
    {"bytes","sha256"} single-DDB shape when xmb_files is empty, or
    {"files": {"ddb": {...}, "<xmbname>": {...}, ...}} when it is not.
    Every Set A-E golden keeps the single-DDB shape (run_reference itself
    never collects XMB files - see reference.py's module docstring); Set
    F's two flagged (-x) jobs are the first callers with a non-empty
    xmb_files, routed through run_reference_from_json instead (CARRY 1,
    task-4-brief.md, curated_jobs()'s own call site above).
    """
    if not xmb_files:
        return {"sha256": hashlib.sha256(ddb).hexdigest(), "bytes": len(ddb)}
    files = {"ddb": {"sha256": hashlib.sha256(ddb).hexdigest(), "bytes": len(ddb)}}
    for name, data in sorted(xmb_files.items()):
        files[name] = {"sha256": hashlib.sha256(data).hexdigest(),
                       "bytes": len(data)}
    return {"files": files}


def entry_files(entry: dict) -> dict[str, dict]:
    """Normalises one manifest.json value to {name: {"bytes","sha256"}},
    the shape both gen_goldens.py --check and verify.py's self_check/
    from_json_check iterate over. A "files" key means the multi-file
    shape already carries this mapping directly; its absence means the
    old-shape single-DDB entry, folded into a one-entry mapping under the
    fixed name "ddb" so every caller can iterate uniformly regardless of
    which shape a given entry has.
    """
    if "files" in entry:
        return entry["files"]
    return {"ddb": entry}


def write_golden_files(fixture: str, slug: str, ddb: bytes,
                       xmb_files: dict[str, bytes], *,
                       root: Path = GOLDENS) -> None:
    """Writes every file of one golden entry to disk under root/fixture/,
    named via golden_file_path - "ddb" for the primary database plus each
    XMB companion's own name."""
    for name, data in {"ddb": ddb, **xmb_files}.items():
        path = golden_file_path(fixture, slug, name, root=root)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify without rewriting")
    args = ap.parse_args()

    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    jobs = curated_jobs()
    manifest = {}
    failures = []

    # In check mode the committed manifest is itself under test. CI trusts
    # manifest.json, so a stale entry sitting beside a correct golden must
    # fail here rather than pass silently.
    committed = {}
    if args.check:
        if MANIFEST.exists():
            committed = json.loads(MANIFEST.read_text(encoding="utf-8"))
        else:
            failures.append("manifest.json is missing")

    for fixture, combo in jobs:
        dsf = FIXTURES / f"{fixture}.DSF"
        if not dsf.exists():
            failures.append(f"{fixture}: fixture missing at {dsf}")
            continue

        # CARRY 1 (task-4-brief.md): run_reference stays deliberately
        # XMB-blind (reference.py's module docstring) so CONDACTS's
        # incidental 0.XMB write can never flip its four plain, single-
        # file goldens over to the multi-file shape. Set F's flagged
        # jobs (flags=("-x",)) are the first that must collect XMB
        # companions during GENERATION, so they alone are routed through
        # run_reference_from_json instead - it already collects *.XMB
        # (built for the --from-json sweep, reference.py). Its extra -v/
        # --from-json-shaped DRB invocation changes only DRB's own
        # stdout, never the DDB/XMB bytes it writes, so reusing it here
        # for goldens is safe; this keeps a single XMB-collecting code
        # path rather than adding a second one. Plain (unflagged) jobs
        # keep using run_reference and stay XMB-blind, exactly as
        # before.
        if combo.flags:
            fj = run_reference_from_json(cfg, dsf, combo)
            ok, ddb = fj.ok, fj.ddb
            stage, stdout, stderr = fj.stage, fj.stdout, fj.stderr
            xmb_files: dict[str, bytes] = fj.xmb_files if fj.ok else {}
        else:
            result = run_reference(cfg, dsf, combo)
            ok, ddb = result.ok, result.ddb
            stage, stdout, stderr = result.stage, result.stdout, result.stderr
            xmb_files = {}

        if not ok:
            failures.append(
                f"{fixture}/{combo.slug}: reference failed at {stage}\n"
                f"  {stderr.strip() or stdout.strip()}")
            continue

        key = f"{fixture}/{combo.slug}"
        entry = manifest_entry(ddb, xmb_files)
        manifest[key] = entry

        if args.check:
            for name, file_meta in entry_files(entry).items():
                data = ddb if name == "ddb" else xmb_files[name]
                path = golden_file_path(fixture, combo.slug, name)
                if not path.exists():
                    failures.append(f"{key} ({name}): golden missing")
                elif path.read_bytes() != data:
                    failures.append(f"{key} ({name}): golden differs from reference")

            committed_entry = committed.get(key)
            if committed_entry is None:
                failures.append(f"{key}: absent from manifest.json")
            elif committed_entry != entry:
                failures.append(f"{key}: manifest entry is stale")
        else:
            write_golden_files(fixture, combo.slug, ddb, xmb_files)

        print(f"  {fixture}/{combo.slug}: {len(ddb)} bytes")

    if args.check:
        for orphan in sorted(set(committed) - set(manifest)):
            failures.append(
                f"{orphan}: in manifest.json but not produced by curated_jobs")
    else:
        if failures:
            print("manifest.json NOT written - one or more jobs failed. "
                  "The previous manifest is left in place.")
        else:
            MANIFEST.parent.mkdir(parents=True, exist_ok=True)
            MANIFEST.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")

    print(f"\n{len(jobs)} jobs, {len(failures)} failures")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
