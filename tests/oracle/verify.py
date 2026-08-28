# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Gibson.
"""Verifies ndrc output against the committed goldens.

Needs no reference toolchain and no PHP, which is what lets CI run it.

  python verify.py --ndrc ../../ndrc.exe

Until ndrc can compile anything, --self-check verifies that the goldens
are internally consistent with their manifest, so the harness itself is
under test from day one.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple, Sequence

sys.path.insert(0, str(Path(__file__).parent))

from gen_goldens import (
    curated_jobs, golden_file_path, entry_files,
)
from ndrcoracle.config import (
    ALL_TARGET_SUBTARGET_PAIRS, ConfigError, load_config, layout_for,
)
from ndrcoracle.diffreport import format_diff
from ndrcoracle.divergence import load_registry, unauthorised_differences
from ndrcoracle.matrix import Combo
from ndrcoracle.reference import (
    TIMEOUT_SECONDS, run_reference_from_json, run_drf_only, stage_run_dir,
    collect_xmb_files, collect_jddb_files,
)

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "fixtures"
GOLDENS = ROOT / "goldens"
MANIFEST = GOLDENS / "manifest.json"

# The 13 header words DRB patches at drb.php:2039-2072 (analysis S4.1),
# in file order. Values are ABSOLUTE addresses; for NEXTDAAD the base
# address is 0, so an absolute address and a file offset coincide, which
# is what lets these double as section boundaries into the DDB bytes.
HEADER_PATCH_FIELDS = [
    ("tokens", 8),
    ("process list", 10),
    ("object lookup", 12),
    ("location lookup", 14),
    ("user msg lookup", 16),
    ("sys msg lookup", 18),
    ("connections lookup", 20),
    ("vocabulary", 22),
    ("initially-at", 24),
    ("object names", 26),
    ("weight/attr", 28),
    ("extra attr", 30),
    ("end address", 32),
]


def combo_from_slug(slug: str) -> Combo:
    """Parses a slug back into a Combo.

    Shape is TARGET[_SUBTARGET]_LANG_vN_MODE, and the last three fields
    are fixed, so everything before them is the target and optional
    subtarget. Subtargets such as 5_6 contain underscores, which is why
    this parses from the right.
    """
    parts = slug.split("_")
    mode = parts[-1]
    version = parts[-2]
    lang = parts[-3]
    head = parts[:-3]
    target = head[0]
    subtarget = "_".join(head[1:]) if len(head) > 1 else None
    return Combo(target=target, subtarget=subtarget, lang=lang,
                 v3=(version == "v3"), classic=(mode == "classic"))


def self_check() -> int:
    if not MANIFEST.exists():
        print(f"ERROR: {MANIFEST} not found. Run gen_goldens.py first.")
        return 2
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    failures = []
    for key, meta in sorted(manifest.items()):
        fixture, slug = key.split("/", 1)
        # entry_files normalises old-shape {"bytes","sha256"} entries to
        # {"ddb": meta} and new-shape {"files": {...}} entries to their
        # own mapping, so every file of a multi-file entry gets checked
        # here too (task-2-brief.md Interfaces: "self-check ... verify
        # every file of an entry").
        for name, file_meta in entry_files(meta).items():
            path = golden_file_path(fixture, slug, name)
            if not path.exists():
                failures.append(f"{key} ({name}): golden file missing")
                continue
            data = path.read_bytes()
            if len(data) != file_meta["bytes"]:
                failures.append(
                    f"{key} ({name}): {len(data)} bytes, manifest says "
                    f"{file_meta['bytes']}")
            digest = hashlib.sha256(data).hexdigest()
            if digest != file_meta["sha256"]:
                failures.append(f"{key} ({name}): sha256 mismatch")

    # The loop above walks the manifest, so a golden file on disk that no
    # manifest entry mentions would otherwise be invisible. Walk the other
    # direction too.
    on_disk = {f"{path.parent.name}/{path.stem}"
               for path in GOLDENS.rglob("*.ddb")}
    for orphan in sorted(on_disk - set(manifest)):
        failures.append(
            f"{orphan}: golden file present but absent from manifest.json")

    print(f"{len(manifest)} goldens, {len(failures)} failures")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


def verify_ndrc(ndrc: Path) -> int:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    registry = load_registry()
    failures = []
    skipped = []

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        for key in sorted(manifest):
            fixture, slug = key.split("/", 1)
            entry = manifest[key]

            # CARRY 2 (task-2-review.md, resolved task-4-brief.md): this
            # plain --ndrc path runs ndrc's own DSF-driven CLI shape (no
            # --from-json - see the module docstring above and
            # docs/dev/records/task-9-report.md's own noted concern that this
            # shape has no ndrc counterpart yet), and combo_from_slug
            # below never reconstructs a slug's own flags (it only parses
            # target/subtarget/lang/version/mode), so it has no way to
            # pass a flagged golden's own flags (e.g. Set F's -x) back to
            # ndrc, and no XMB comparison of its own either. Comparing a
            # multi-file entry's "ddb" alone here would silently skip its
            # XMB companions AND run ndrc without the flag that produced
            # them - not a meaningful check either way - so multi-file
            # entries are skipped explicitly, with a printed note, rather
            # than silently passing or wrongly failing. Their real,
            # flag-aware, XMB-aware gate is
            # verify.py --from-json --only (from_json_check, above),
            # which is what task-4-brief.md's own Gates section runs.
            if "files" in entry:
                skipped.append(key)
                print(f"SKIP {key}: multi-file entry (XMB companions) - "
                      f"verify_ndrc has no flag/XMB path; see "
                      f"--from-json --only for this entry's real gate")
                continue

            combo = combo_from_slug(slug)
            golden = (GOLDENS / fixture / f"{slug}.ddb").read_bytes()
            out = tmpdir / f"{fixture}_{slug}.ddb"

            args = [str(ndrc), *combo.drf_args, combo.lang,
                    str(FIXTURES / f"{fixture}.DSF"), str(out)]
            if combo.v3:
                args.append("-v3")
            if combo.classic:
                args.append("-c")
            args.extend(combo.flags)

            proc = subprocess.run(args, capture_output=True, text=True,
                                  errors="replace")
            if proc.returncode != 0:
                failures.append(f"{key}: ndrc exited {proc.returncode}\n"
                                f"  {proc.stderr.strip()}")
                continue
            if not out.exists():
                failures.append(f"{key}: ndrc produced no output")
                continue

            produced = out.read_bytes()
            if produced == golden:
                continue

            unauthorised = unauthorised_differences(
                registry, fixture, combo, golden, produced)
            if not unauthorised:
                print(f"  {key}: divergence fully authorised by the registry")
                continue

            detail = "".join(
                f"  unauthorised 0x{start:04X}..0x{end:04X}: {why}\n"
                for start, end, why in unauthorised)
            failures.append(f"{key}:\n" + detail
                            + format_diff(golden, produced))

    print(f"\n{len(manifest)} goldens, {len(failures)} failures"
          + (f", {len(skipped)} skipped (multi-file)" if skipped else ""))
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


def build_section_map(expected_ddb: bytes, *, big_endian: bool = False,
                       base_address: int = 0) -> list[tuple[str, int]]:
    """Section boundaries for diffreport, read from the EXPECTED file's own
    header patch words (analysis S4.1) plus ("header", 0) for everything
    before the first patched pointer. Sorted by start address because the
    fields are not written in address order (S12/S6.6: the object lookup
    pointer is offset backwards from the object data it points into, so it
    does not sit where its position in the patch table would suggest).

    Header words are ABSOLUTE addresses (spec S3.2/S3.3), so converting one
    to a file offset needs the target's own base_address subtracted back
    out, and reading it needs the target's own byte order. Phase 1a hard-
    coded both to their NEXTDAAD values (0, little-endian); Phase 1b passes
    them in from ndrcoracle.config.layout_for(target, subtarget) at every
    call site instead.
    """
    endian = ">" if big_endian else "<"
    entries = [("header", 0)]
    for name, offset in HEADER_PATCH_FIELDS:
        addr = struct.unpack_from(f"{endian}H", expected_ddb, offset)[0]
        entries.append((name, addr - base_address))
    entries.sort(key=lambda e: e[1])
    return entries


def _normalise_newlines(s: str) -> str:
    """CRLF/CR -> LF, so a Windows-toolchain transcript and a POSIX one
    compare equal when their content is otherwise identical."""
    return s.replace("\r\n", "\n").replace("\r", "\n")


def _strip_banner_line(stdout: str) -> str:
    """Drops a transcript's own unconditional first stdout line - each
    tool prints its own product/version banner before doing anything
    else (ndrc: 'NDRC 0.1 --from-json', main.c:223; DRB: 'DAAD Reborn
    Compiler Backend M.N (C) Uto YYYY', drb.php:1709), and neither
    banner has a counterpart on the other side, so the (c) stdout
    comparison strips both, symmetrically, before comparing what
    follows. Input is assumed already newline-normalised."""
    _, _, rest = stdout.partition("\n")
    return rest


# DRF injects YEARHIGH, YEARLOW, MONTH and DAY into the JSON's symbol
# list from the live system clock (drf.pas:259-262), so a byte
# comparison against a reference run taken at a different moment (or
# against the committed fixture, frozen at whatever moment IT was
# generated) needs these four Value numbers neutralised first - spec
# section 6(a). The shape matched, {"symbol":"YEARHIGH", "Value":20} -
# no space after the first colon, one space after the comma - is
# transcribed from the real committed tests/fixtures/
# BLANK_EN.NEXTDAAD_EN_v3.json bytes, not approximated; nothing else in
# the JSON is touched (key order, whitespace, CRLF, escaping stay part
# of the gate).
_CLOCK_SYMBOL_RE = re.compile(
    r'(\{"symbol":"(?:YEARHIGH|YEARLOW|MONTH|DAY)", "Value":)\d+(\})')


def _normalise_clock_symbols(text: str) -> str:
    """Replaces the four clock symbols' own Value numbers with 0, and
    nothing else - see _CLOCK_SYMBOL_RE's comment for the exact shape
    and rationale. Idempotent (already-zeroed input is unchanged)."""
    return _CLOCK_SYMBOL_RE.sub(r"\g<1>0\g<2>", text)


def _pair_selected(fixture: str, slug: str, only: list[str]) -> bool:
    """AND semantics: every --only substring must appear in "fixture/slug".
    An empty `only` selects every pair."""
    haystack = f"{fixture}/{slug}"
    return all(substr in haystack for substr in only)


def _strtol_c10(s: str) -> int:
    """Mirrors C's strtol(s, NULL, 10) (main.c:431's decimal arm):
    optional leading whitespace, optional sign, then as many decimal
    digits as are present; 0 when none are found. Trailing junk is
    ignored rather than an error - unlike Python's int(), which raises
    on it - matching intval()'s stop-at-first-non-digit semantics that
    main.c:425-430's PORT NOTE pins as equivalent here."""
    i, n = 0, len(s)
    while i < n and s[i] in " \t\n\r\v\f":
        i += 1
    sign = 1
    if i < n and s[i] in "+-":
        if s[i] == "-":
            sign = -1
        i += 1
    start = i
    while i < n and s[i].isdigit():
        i += 1
    return sign * int(s[start:i]) if i > start else 0


def _base_override_from_flags(flags: tuple[str, ...]) -> int | None:
    """Parses a combo's own -b=... flag (case-insensitive - main.c
    upper-cases options before dispatch, see test_cli.py case 8's
    "-B=0X100") into an integer base address for layout_for's
    base_override, or None when the combo carries no such flag
    (task-2-brief.md Interfaces: "the sweep parses -b=-style flags to
    supply it when building section maps").

    Mirrors main.c:391-433's -B= arm exactly (not Python's int(...,
    0), which accepts 0o/0b prefixes DRB never would and raises on
    forms intval() parses, e.g. a leading-zero decimal): a case-
    matched "0X" prefix takes the hexdec()-style arm - every non-hex
    character deleted from the remainder first, then the leftover
    parsed as one hex run, 0 when nothing is left (main.c:403-423);
    otherwise the intval()-equivalent decimal arm, strtol(value, NULL,
    10) semantics (main.c:431, _strtol_c10 above). This helper only
    feeds diff-section attribution, never a value that reaches ndrc or
    DRB, so it must never disagree with what the binary itself
    computes."""
    for flag in flags:
        upper = flag.upper()
        if upper[:3] != "-B=":
            continue
        value = upper[3:]
        if value[:2] == "0X":
            hexdigits = "".join(
                c for c in value[2:] if c in "0123456789ABCDEF")
            return int(hexdigits, 16) if hexdigits else 0
        return _strtol_c10(value)
    return None


def _stage_ndrc_run_dir(run_dir: Path, json_bytes: bytes, fixture: str) -> None:
    """Stages one ndrc --from-json run directory: writes g.json, then the
    sidecar hook - copies tests/fixtures/<FIXTURE>.tok to g.tok when one
    exists, mirroring reference.py's stage_run_dir - then the EXTERNS
    asset hook (task-3, same mechanism as reference.py's
    stage_extern_assets: every tests/fixtures/EXT_*.BIN, glob-matched,
    copied in whenever fixture is "EXTERNS" - ndrc resolves extern paths
    against its own cwd exactly as DRB does), then the INCLUDE asset hook
    (task-5, same mechanism as reference.py's stage_include_assets:
    INCLUDE2.DSF and INC_DATA.BIN, name-matched, copied in whenever
    fixture is "INCLUDE" - these are only ever relevant to a --to-json
    compile, never to --from-json's own JSON-only input, but the hook is
    mirrored here anyway so the two staging functions stay in lockstep),
    then the same stale-XMB guard (task-2-brief.md Interfaces): run_dir
    is always freshly made by tempfile.TemporaryDirectory, so this always
    holds today; the assert pins it as a hard failure instead of an
    unstated invariant.
    """
    (run_dir / "g.json").write_bytes(json_bytes)
    sidecar = FIXTURES / f"{fixture}.tok"
    if sidecar.exists():
        shutil.copyfile(sidecar, run_dir / "g.tok")
    if fixture == "EXTERNS":
        for asset in sorted(FIXTURES.glob("EXT_*.BIN")):
            shutil.copyfile(asset, run_dir / asset.name)
    if fixture == "INCLUDE":
        for name in ("INCLUDE2.DSF", "INC_DATA.BIN"):
            asset = FIXTURES / name
            if asset.exists():
                shutil.copyfile(asset, run_dir / name)
    stale = sorted(p.name for p in run_dir.glob("*.XMB"))
    assert not stale, (
        f"stale *.XMB found in freshly staged run dir {run_dir}: {stale} "
        f"(spec section 2 guarantee violated)")


def _check_ddb_diff(tag: str, key: str, expected: bytes, produced: bytes,
                    fixture: str, combo: Combo, registry: object,
                    big_endian: bool, base_address: int,
                    label_a: str) -> str | None:
    """Common body of from_json_check's (a) and (b) comparisons: ndrc's
    produced DDB against one expected DDB (the committed golden for (a),
    the fresh reference run for (b)). A byte difference fully covered by
    the divergence registry passes silently, with the same printed note
    verify_ndrc's own byte comparison gives on the same case; anything
    left unauthorised comes back as pair-failure text with a section-
    annotated diff. Returns None when there is nothing to fail on.
    """
    if produced == expected:
        return None
    unauthorised = unauthorised_differences(
        registry, fixture, combo, expected, produced)
    if not unauthorised:
        print(f"  {key}: ({tag}) divergence fully authorised by the registry")
        return None
    section_map = build_section_map(
        expected, big_endian=big_endian, base_address=base_address)
    detail = "".join(
        f"  unauthorised 0x{s:04X}..0x{e:04X}: {why}\n"
        for s, e, why in unauthorised)
    return (f"({tag}) ndrc != {label_a}:\n" + detail
            + format_diff(expected, produced, label_a=label_a,
                          label_b="ndrc", section_map=section_map))


def from_json_check(ndrc: Path, only: list[str] | None = None) -> int:
    """Maintainer gate: needs the reference toolchain (oracle.local.json).

    Sweeps every (fixture, combo) pair gen_goldens.curated_jobs() curates -
    the same set the committed goldens come from - optionally restricted by
    `only` (repeatable --only on the CLI, AND semantics: every substring
    given must appear in "fixture/slug"; see _pair_selected).

    For each selected pair: a FRESH reference DRF+DRB -v run (not the
    committed fixture JSON) supplies the JSON ndrc is fed, DRB's own
    stdout, and DRB's run directory's *.XMB files. ndrc runs --from-json
    -v on a copy of that same fresh JSON, in its own temp directory. Four
    comparisons follow:

      (a) ndrc's files equal the committed golden entry's own files (with
          the golden's own manifest.json entry sanity-checked at the same
          time) - "ddb" always, plus each XMB companion when the entry is
          the multi-file shape (task-2-brief.md Interfaces: "self-check
          and the sweep verify every file of an entry");
      (b) ndrc's DDB equals the fresh reference DDB;
      (c) ndrc's stdout equals DRB's own stdout, after both are newline-
          normalised and each has its own first-line product banner
          stripped (NDRC's "NDRC 0.1 --from-json", DRB's "DAAD Reborn
          Compiler Backend ..." - neither has a counterpart on the other
          side, so neither belongs in the comparison; see
          _strip_banner_line);
      (d) the set of *.XMB files ndrc's run directory holds equals the
          reference run directory's set, name for name and byte for byte
          (empty == empty passes trivially, which is everything until a
          later task teaches ndrc to write XMB files at all);
      (e) same as (d), for *.jddb files (task-7-brief.md Step 5) - HTML
          targets write one per run, everything else's set stays empty on
          both sides.

    (a) and (b) are run through the divergence registry exactly as
    verify_ndrc's own byte comparisons are: a difference fully covered by
    a registry entry does not fail the pair. Section attribution for any
    reported diff uses build_section_map with (base_address, big_endian)
    from ndrcoracle.config.layout_for(combo.target, combo.subtarget,
    base_override) - base_override comes from combo's own -b=... flag
    when it carries one (see _base_override_from_flags).

    combo.flags is appended verbatim to both the reference DRB invocation
    (inside run_reference_from_json) and ndrc's own --from-json
    invocation here, so a flag-carrying combo exercises both sides
    identically. Run-dir staging (_stage_ndrc_run_dir) also carries the
    sidecar hook - tests/fixtures/<FIXTURE>.tok copied in as g.tok when
    it exists - and the stale-XMB guard, matching reference.py's own
    stage_run_dir.

    Most pairs fail until later tasks land - that is what --only is for.
    Prints one PASS/FAIL line per selected pair plus a final tally, and
    returns nonzero iff any selected pair actually ran and failed (a
    reference-toolchain failure short-circuits with exit 2, same as
    before this pair had a sweep to belong to).
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    only = only or []
    registry = load_registry()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8")) \
        if MANIFEST.exists() else {}

    n_selected = 0
    n_passed = 0
    failures: list[str] = []

    for fixture, combo in curated_jobs():
        key = f"{fixture}/{combo.slug}"
        if not _pair_selected(fixture, combo.slug, only):
            continue
        n_selected += 1

        dsf = FIXTURES / f"{fixture}.DSF"
        ref = run_reference_from_json(cfg, dsf, combo)
        if not ref.ok:
            print(f"FAIL {key}")
            failures.append(
                f"{key}: reference failed at {ref.stage}\n"
                f"  {ref.stderr.strip() or ref.stdout.strip()}")
            continue

        base_override = _base_override_from_flags(combo.flags)
        base_address, big_endian = layout_for(
            combo.target, combo.subtarget, base_override)
        pair_failures: list[str] = []

        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            _stage_ndrc_run_dir(tmpdir, ref.json_bytes, fixture)
            out_path = tmpdir / "g.DDB"

            args = [str(ndrc), "--from-json", combo.target]
            if combo.subtarget:
                args.append(combo.subtarget)
            args += [combo.lang, "g.json", "g.DDB", "-v"]
            if combo.classic:
                args.append("-c")
            args.extend(combo.flags)

            proc = subprocess.run(args, cwd=str(tmpdir),
                                  capture_output=True, text=True,
                                  errors="replace")

            if proc.returncode != 0:
                pair_failures.append(
                    f"ndrc exited {proc.returncode}\n  {proc.stderr.strip()}")
            elif not out_path.exists():
                pair_failures.append("ndrc produced no output")
            else:
                produced = out_path.read_bytes()
                ndrc_xmb = collect_xmb_files(tmpdir)

                # (a) ndrc's files vs the committed golden entry - every
                # file of it (task-2-brief.md Interfaces): "ddb" via the
                # same divergence-registry-aware compare as before, plus
                # each XMB companion compared byte-for-byte against
                # ndrc's own produced XMB output.
                entry = manifest.get(key)
                if entry is None:
                    pair_failures.append(f"(a) {key} absent from manifest.json")
                else:
                    for name, file_meta in entry_files(entry).items():
                        fpath = golden_file_path(fixture, combo.slug, name)
                        if not fpath.exists():
                            pair_failures.append(f"(a) golden missing at {fpath}")
                            continue
                        golden_bytes = fpath.read_bytes()
                        if (file_meta.get("sha256")
                                != hashlib.sha256(golden_bytes).hexdigest()
                                or file_meta.get("bytes") != len(golden_bytes)):
                            pair_failures.append(
                                f"(a) manifest.json entry for {key} ({name}) "
                                f"is stale against the golden on disk")
                        if name == "ddb":
                            failure = _check_ddb_diff(
                                "a", key, golden_bytes, produced, fixture, combo,
                                registry, big_endian, base_address, "golden")
                        else:
                            ndrc_bytes = ndrc_xmb.get(name)
                            failure = None
                            if ndrc_bytes != golden_bytes:
                                failure = (
                                    f"(a) ndrc {name} != golden {name}:\n"
                                    + format_diff(golden_bytes, ndrc_bytes or b"",
                                                  label_a=f"golden {name}",
                                                  label_b=f"ndrc {name}"))
                        if failure:
                            pair_failures.append(failure)

                # (b) ndrc DDB vs fresh reference DDB.
                failure = _check_ddb_diff(
                    "b", key, ref.ddb, produced, fixture, combo,
                    registry, big_endian, base_address, "fresh reference")
                if failure:
                    pair_failures.append(failure)

                # (c) ndrc stdout vs DRB's own stdout, each with its own
                # first-line product banner stripped (see
                # _strip_banner_line).
                ndrc_body = _strip_banner_line(_normalise_newlines(proc.stdout))
                ref_body = _strip_banner_line(_normalise_newlines(ref.drb_stdout))
                if ndrc_body != ref_body:
                    pair_failures.append(
                        "(c) stdout differs:\n"
                        + format_diff(ref_body.encode("utf-8"),
                                      ndrc_body.encode("utf-8"),
                                      label_a="drb stdout (banner stripped)",
                                      label_b="ndrc stdout (banner stripped)"))

                # (d) *.XMB files ndrc's run produced vs the reference's.
                if ndrc_xmb != ref.xmb_files:
                    names = sorted(set(ndrc_xmb) | set(ref.xmb_files))
                    detail = []
                    for name in names:
                        n_bytes = ndrc_xmb.get(name)
                        r_bytes = ref.xmb_files.get(name)
                        if n_bytes != r_bytes:
                            detail.append(
                                f"  {name}: reference="
                                f"{'absent' if r_bytes is None else f'{len(r_bytes)} bytes'}"
                                f", ndrc="
                                f"{'absent' if n_bytes is None else f'{len(n_bytes)} bytes'}")
                    pair_failures.append(
                        "(d) XMB files differ:\n" + "\n".join(detail))

                # (e) *.jddb files ndrc's run produced vs the reference's.
                ndrc_jddb = collect_jddb_files(tmpdir)
                if ndrc_jddb != ref.jddb_files:
                    names = sorted(set(ndrc_jddb) | set(ref.jddb_files))
                    detail = []
                    for name in names:
                        n_bytes = ndrc_jddb.get(name)
                        r_bytes = ref.jddb_files.get(name)
                        if n_bytes != r_bytes:
                            detail.append(
                                f"  {name}: reference="
                                f"{'absent' if r_bytes is None else f'{len(r_bytes)} bytes'}"
                                f", ndrc="
                                f"{'absent' if n_bytes is None else f'{len(n_bytes)} bytes'}")
                    pair_failures.append(
                        "(e) jddb files differ:\n" + "\n".join(detail))

        if pair_failures:
            print(f"FAIL {key}")
            failures.append(f"{key}:\n" + "\n".join(pair_failures))
        else:
            print(f"PASS {key}")
            n_passed += 1

    print(f"\n{n_selected} pairs run, {n_passed} passed, "
         f"{len(failures)} failed")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


def to_json_compare_files(a_path: Path, b_path: Path) -> int:
    """CI leg of THE PAIR GATE (spec section 6): CI has no reference
    toolchain, so instead of a fresh drf.exe run it compares two
    already-produced JSON files after clock-symbol normalisation only -
    the same _normalise_clock_symbols the maintainer gate's own (a)
    comparison uses (to_json_check above), so CI and the maintainer
    gate share one normalisation implementation rather than two.

    Wired into ci.yml's backend job: ndrc --to-json's own output for
    the committed BLANK_EN.DSF fixture against the committed
    tests/fixtures/BLANK_EN.NEXTDAAD_EN_v3.json fixture. Prints
    PASS/FAIL and returns 0/1.
    """
    a = _normalise_clock_symbols(a_path.read_text(encoding="latin-1"))
    b = _normalise_clock_symbols(b_path.read_text(encoding="latin-1"))
    if a == b:
        print(f"PASS to-json-compare: {a_path} == {b_path} "
             "(post clock normalisation)")
        return 0
    print(f"FAIL to-json-compare: {a_path} != {b_path} "
         "(post clock normalisation)")
    print(format_diff(a.encode("latin-1"), b.encode("latin-1"),
                      label_a=str(a_path), label_b=str(b_path)))
    return 1


# The composition check's own fixture list, extending the original
# BLANK_EN-only check to the three corpus-join fixtures that HAVE a
# NEXTDAAD_EN_v3_opt golden to compose against - EXPR and IFDEFS compile
# cleanly through the full --to-json | --from-json pipeline, and INCLUDE
# does too once its own INCLUDE2.DSF/INC_DATA.BIN sidecars are staged
# alongside it (handled by stage_run_dir inside _composition_leg, the
# SAME helper gen_goldens.py and the --from-json sweep use). XMSG is
# deliberately absent: it has NO committed DDB golden to compose against
# at all - its `XDATA "xdata payload"` condact is valid --to-json JSON
# but the XDATA_OPCODE backend rewrite (drb.php:958-997, ported at
# emit_proc.c:268) refuses to build a DDB from non-numeric other_strings
# text, "There is not data enough in XDATA condact." - measured
# identical on reference drb.php and ndrc --from-json 2026-08-27 (see
# gen_goldens.py's curated_jobs() Set K).
COMPOSITION_EXTRA_FIXTURES: tuple[str, ...] = ("EXPR", "IFDEFS", "INCLUDE")


def _composition_leg(ndrc: Path, fixture: str) -> str | None:
    """One fixture's own composition leg, entirely self-contained: stages
    tests/fixtures/<FIXTURE>.DSF fresh via stage_run_dir (so INCLUDE's own
    sidecars, or a .tok override, come along exactly as they do for
    gen_goldens.py and the --from-json sweep), runs `ndrc --to-json
    NEXTDAAD g.DSF g.json -v3` there, then `ndrc --from-json NEXTDAAD EN
    g.json comp.ddb` on the JSON ndrc itself just wrote, and byte-compares
    the result against tests/goldens/<FIXTURE>/NEXTDAAD_EN_v3_opt.ddb.

    Returns None when the composition holds, else one failure string.
    Needs no reference toolchain at all - unlike to_json_check's own (a)/
    (b)/(c) legs, this never runs drf.exe; it only tests ndrc's own front
    end against its own back end.
    """
    dsf = FIXTURES / f"{fixture}.DSF"
    golden_path = GOLDENS / fixture / "NEXTDAAD_EN_v3_opt.ddb"

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        stage_run_dir(tmpdir, dsf)

        to_json_args = [str(ndrc), "--to-json", "NEXTDAAD", "g.DSF",
                        "g.json", "-v3"]
        p1 = subprocess.run(to_json_args, cwd=str(tmpdir),
                            capture_output=True, text=True, errors="replace")
        json_path = tmpdir / "g.json"
        if p1.returncode != 0 or not json_path.exists():
            return (f"(composition {fixture}) ndrc --to-json exited "
                    f"{p1.returncode}\n  {p1.stderr.strip()}")

        from_json_args = [str(ndrc), "--from-json", "NEXTDAAD", "EN",
                          "g.json", "comp.ddb"]
        p2 = subprocess.run(from_json_args, cwd=str(tmpdir),
                            capture_output=True, text=True, errors="replace")
        comp_out = tmpdir / "comp.ddb"
        if p2.returncode != 0:
            return (f"(composition {fixture}) ndrc --from-json exited "
                    f"{p2.returncode}\n  {p2.stderr.strip()}")
        if not comp_out.exists():
            return (f"(composition {fixture}) ndrc --from-json produced "
                    f"no output")

        produced = comp_out.read_bytes()
        golden_bytes = golden_path.read_bytes()
        if produced != golden_bytes:
            return (f"(composition {fixture}) ndrc's round-tripped DDB != "
                    f"golden:\n"
                    + format_diff(golden_bytes, produced, label_a="golden",
                                  label_b="ndrc round-trip"))
    return None


def to_json_check(ndrc: Path) -> int:
    """THE PAIR GATE (task-10-brief.md, spec section 6): verify.py's
    --to-json mode. Stages BLANK_EN.DSF fresh, runs reference drf.exe
    and ndrc --to-json with IDENTICAL args (NEXTDAAD, -v3, -verbose),
    and requires:

      (a) JSON byte-identical after clock-symbol normalisation (see
          _normalise_clock_symbols) - nothing else canonicalised.
      (b) stdout identical after stripping each side's own first-line
          product banner (drf.exe: "DAAD Reborn Compiler Frontend ...";
          ndrc: "NDRC 0.1 --to-json" - neither has a counterpart on the
          other side, same rationale as from_json_check's own (c)).
      (c) exit codes identical.

    Plus the COMPOSITION check: ndrc's own JSON, fed straight to the
    EXISTING ndrc --from-json back end (NEXTDAAD EN), must reproduce
    tests/goldens/BLANK_EN/NEXTDAAD_EN_v3_opt.ddb byte-identically -
    front and back proven compatible ahead of Phase 3 joining them. Then
    the SAME composition property, independently checked (own fresh
    --to-json run, own --from-json run) for every fixture in
    COMPOSITION_EXTRA_FIXTURES - see _composition_leg and that tuple's
    own comment for why XMSG is not among them.

    Needs the reference toolchain (oracle.local.json) - this is the
    maintainer gate. CI has no reference toolchain, so it instead
    compares ndrc's own --to-json output against the committed fixture
    JSON directly (see ci.yml's backend job, which reuses
    _normalise_clock_symbols through this same module).
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    combo = Combo(target="NEXTDAAD", subtarget=None, lang="EN",
                 v3=True, classic=False)
    dsf = FIXTURES / "BLANK_EN.DSF"
    key = "BLANK_EN/NEXTDAAD (to-json)"

    ref = run_drf_only(cfg, dsf, combo, extra_args=("-verbose",))
    if not ref.ok:
        print(f"FAIL {key}")
        print(f"  reference drf.exe failed at stage {ref.stage}\n"
              f"  {ref.stderr.strip() or ref.stdout.strip()}")
        print("\nto_json gate: 1 pair run, 0 passed, 1 failed")
        return 1

    failures: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        run_dsf = tmpdir / "g.DSF"
        shutil.copyfile(dsf, run_dsf)
        out_json = tmpdir / "g.json"

        # Run with cwd=tmpdir and relative "g.DSF"/"g.json" args, exactly
        # mirroring run_drf_only's own reference invocation (which runs
        # the same way, cwd=run_dir) - drf.exe's "Reading g.DSF" stdout
        # line echoes the argument as given, so an absolute path here
        # would diverge (b) on the path alone, not a real behaviour gap.
        args = [str(ndrc), "--to-json", "NEXTDAAD", "g.DSF",
                "g.json", "-v3", "-verbose"]
        proc = subprocess.run(args, cwd=str(tmpdir), capture_output=True,
                              text=True, errors="replace")

        ndrc_json = out_json.read_bytes() if out_json.exists() else None

        # (a) JSON byte-identical after clock normalisation.
        if ndrc_json is None:
            failures.append("(a) ndrc --to-json produced no JSON output")
        else:
            ref_norm = _normalise_clock_symbols(
                ref.json_bytes.decode("latin-1"))
            ndrc_norm = _normalise_clock_symbols(ndrc_json.decode("latin-1"))
            if ref_norm != ndrc_norm:
                failures.append(
                    "(a) JSON differs after clock normalisation:\n"
                    + format_diff(ref_norm.encode("latin-1"),
                                  ndrc_norm.encode("latin-1"),
                                  label_a="drf.exe (normalised)",
                                  label_b="ndrc (normalised)"))

        # (b) stdout identical after each side's own banner is stripped.
        ndrc_body = _strip_banner_line(_normalise_newlines(proc.stdout))
        ref_body = _strip_banner_line(_normalise_newlines(ref.stdout))
        if ndrc_body != ref_body:
            failures.append(
                "(b) stdout differs:\n"
                + format_diff(ref_body.encode("utf-8"),
                              ndrc_body.encode("utf-8"),
                              label_a="drf.exe stdout (banner stripped)",
                              label_b="ndrc stdout (banner stripped)"))

        # (c) exit codes identical.
        if proc.returncode != ref.returncode:
            failures.append(
                f"(c) exit code differs: reference={ref.returncode}, "
                f"ndrc={proc.returncode}")

        # Composition check: ndrc's own JSON round-tripped through
        # ndrc --from-json must reproduce the committed golden.
        if ndrc_json is not None:
            comp_out = tmpdir / "comp.ddb"
            comp_args = [str(ndrc), "--from-json", "NEXTDAAD", "EN",
                        str(out_json), str(comp_out)]
            comp_proc = subprocess.run(comp_args, cwd=str(tmpdir),
                                       capture_output=True, text=True,
                                       errors="replace")
            golden = (GOLDENS / "BLANK_EN"
                     / "NEXTDAAD_EN_v3_opt.ddb").read_bytes()
            if comp_proc.returncode != 0:
                failures.append(
                    f"(composition) ndrc --from-json exited "
                    f"{comp_proc.returncode}\n  {comp_proc.stderr.strip()}")
            elif not comp_out.exists():
                failures.append(
                    "(composition) ndrc --from-json produced no output")
            else:
                produced = comp_out.read_bytes()
                if produced != golden:
                    failures.append(
                        "(composition) ndrc's round-tripped DDB != golden:\n"
                        + format_diff(golden, produced, label_a="golden",
                                      label_b="ndrc round-trip"))

    # The composition check's own fixture list, extended past BLANK_EN.
    # Each of these runs entirely independently of the (a)/(b)/(c) pair-
    # gate legs above (own --to-json run, own run directory) - see
    # _composition_leg.
    for extra_fixture in COMPOSITION_EXTRA_FIXTURES:
        extra_failure = _composition_leg(ndrc, extra_fixture)
        if extra_failure:
            failures.append(extra_failure)

    if failures:
        print(f"FAIL {key}")
        for f in failures:
            print(f"  {f}")
        print(f"\nto_json gate: 1 pair run, 0 passed, {len(failures)} failed")
        return 1

    print(f"PASS {key}")
    print("\nto_json gate: 1 pair run, 1 passed, 0 failed "
         "((a) JSON match, (b) stdout match, (c) exit codes match, "
         "composition DDB match)")
    return 0


# ---------------------------------------------------------------------------
# THE MATRIX GATE (Phase 2b task 7, spec section 5)
# ---------------------------------------------------------------------------

# The --to-json deck: every committed .DSF fixture except INCLUDE2.DSF (a
# sidecar, never compiled standalone - see reference.stage_include_assets)
# and TOKFILE.DSF. TOKFILE is excluded deliberately: its .tok sidecar is
# consumed by the DRB half of the pipeline (drb.php:1749-1756), so its DSF
# is byte-identical to BLANK_EN.DSF as far as DRF is concerned and every
# cell it could contribute here would duplicate a BLANK_EN cell exactly.
# Whatever --from-json does with TOKFILE is untouched by this.
#
# BADSYNTAX is the one deliberately-invalid fixture (owner-approved): a
# byte-copy of BLANK_EN.DSF with one garbage line ("- ...") spliced in
# immediately before the /CTL section. That "-" is not followed by a
# digit, so DSF.l's `-?[0-9]+` rule cannot match it and it falls through
# to the catch-all `.` rule (DSF.l:57), which is an unconditional lexer
# error hit during the initial token scan, before any target-specific
# processing - the same failure on every target/subtarget/version
# combination, so every one of its cells is a pass-as-pin: reference
# drf.exe fails identically to ndrc (measured on NEXTDAAD v3, PC VGA v3,
# HTML v2 and ZX 48K v2 before wiring this in - all four gave the
# identical "64:2:g.DSF: Unexpected character or string: "-"." with exit
# code 1 and no g.json on either side). It has no --from-json golden (it
# never compiles), so it is not in gen_goldens.py's curated_jobs() and
# carries no committed tests/fixtures/*.json - only this --to-json deck
# exercises it. It must never be "fixed": its entire purpose is to keep
# the matrix's expected-failure pin path under permanent live exercise.
TO_JSON_DECK: tuple[str, ...] = (
    "BLANK_EN", "BLANK_ES", "STARTER", "CONDACTS", "BIGDDB", "EXTERNS",
    "XPLAY", "DEBUG", "IFDEFS", "INCLUDE", "EXPR", "XMSG", "BADSYNTAX",
)

# 13 fixtures x 35 target/subtarget pairs x 2 language versions = 910.
# Generated in code from ndrcoracle.config.ALL_TARGET_SUBTARGET_PAIRS (the
# same 35-row mirror of src/targets.c that carries its own import-time
# count assertion), never from a manifest file, so a 36th target row lands
# in this sweep automatically.
TO_JSON_MATRIX_CELLS = len(TO_JSON_DECK) * len(ALL_TARGET_SUBTARGET_PAIRS) * 2


def to_json_cell_id(fixture: str, target: str, subtarget: str | None,
                    v3: bool) -> str:
    """One matrix cell's id: <FIXTURE>_<TARGET>[_<SUB>]_<v2|v3>, the string
    --only matches against."""
    parts = [fixture, target]
    if subtarget:
        parts.append(subtarget)
    parts.append("v3" if v3 else "v2")
    return "_".join(parts)


def _cell_selected(cell_id: str, only: list[str]) -> bool:
    """AND semantics, same as _pair_selected: every --only substring must
    appear in the cell id. An empty `only` selects every cell."""
    return all(substr in cell_id for substr in only)


def _to_json_argv(exe: Path, target: str, subtarget: str | None,
                  v3: bool, *, ndrc: bool,
                  extra: Sequence[str] = ()) -> list[str]:
    """One side's argv for a matrix cell.

    Both sides run with cwd=<run dir> and the relative names g.DSF/g.json,
    mirroring run_drf_only's own reference invocation: drf.exe echoes the
    input argument verbatim ("Reading g.DSF"), so an absolute path on one
    side alone would diverge the stdout leg on the path text rather than
    on any real behaviour difference.

    ndrc's argv is drf.exe's with the mode flag prepended - `ndrc --to-json
    <TARGET> [SUB] g.DSF g.json [-v3]` against `drf.exe <TARGET> [SUB]
    g.DSF g.json [-v3]` - which is the argv order main.c's run_to_json
    ports from drf.pas:350-413. v2 is the flag's ABSENCE on both sides.

    `extra` appends further trailing arguments IDENTICALLY to both sides -
    the flag-extra runs' own option or symbol list (see
    TO_JSON_EXTRA_JOBS). drf.pas:351-413 consumes everything after the
    output name in one order-free WHILE loop, so trailing is the position
    every extra was measured in; the matrix cells themselves pass none.
    """
    args = [str(exe)]
    if ndrc:
        args.append("--to-json")
    args.append(target)
    if subtarget:
        args.append(subtarget)
    args += ["g.DSF", "g.json"]
    if v3:
        args.append("-v3")
    args += list(extra)
    return args


def _run_to_json_side(argv: list[str], dsf: Path) -> tuple[int, str, bytes | None]:
    """Runs one side of a matrix cell in its own fresh, disposable run
    directory, and returns (exit code, stdout, g.json bytes or None).

    Staging is reference.stage_run_dir - the SAME helper gen_goldens and
    the --from-json sweep use, so the fixture's own sidecars come along by
    exactly the hooks Tasks 3/5 registered there (EXT_*.BIN for EXTERNS,
    INCLUDE2.DSF/INC_DATA.BIN for INCLUDE, <FIXTURE>.tok for a tokenised
    fixture) with no per-cell staging logic duplicated here.

    A missing g.json is reported as None rather than as an error: on a
    cell where the reference itself fails, whether drf.exe left a partial
    file behind is part of what the cell pins, not a harness problem. A
    timeout is reported as exit code -1 with the timeout note appended to
    stdout, so it can never be mistaken for a matching failure on the
    other side.
    """
    with tempfile.TemporaryDirectory() as tmp:
        run_dir = Path(tmp)
        stage_run_dir(run_dir, dsf)
        try:
            proc = subprocess.run(argv, cwd=str(run_dir), capture_output=True,
                                  text=True, errors="replace",
                                  timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return -1, f"TIMED OUT after {TIMEOUT_SECONDS} seconds\n", None
        out = run_dir / "g.json"
        return proc.returncode, proc.stdout, (out.read_bytes()
                                              if out.exists() else None)


def _timeout_leg(ref_rc: int, ndrc_rc: int) -> str | None:
    """The UNCONDITIONAL-FAIL leg for a synthesised timeout exit code.

    _run_to_json_side reports a TimeoutExpired as exit code -1 with the
    real stdout thrown away and replaced by its own "TIMED OUT after N
    seconds" note. That note is a single line, so _strip_banner_line eats
    it and leaves an EMPTY body - which means two timed-out sides compare
    equal on all three legs (rc -1 == -1, "" == "", no g.json on either)
    and would otherwise score as a pinned reference failure. A timeout is
    never a pin: it is the harness failing to obtain a measurement, and
    the fixture/exe pair it was measuring stays unverified. So either
    side reporting -1 fails the cell outright, before any leg comparison
    runs.

    -1 can in principle also reach here from a real child (a POSIX
    SIGHUP kill), which is equally not a pin. Returns the failure text,
    or None when neither side timed out.
    """
    if ref_rc != -1 and ndrc_rc != -1:
        return None
    which = []
    if ref_rc == -1:
        which.append("reference")
    if ndrc_rc == -1:
        which.append("ndrc")
    return (f"(timeout) {' and '.join(which)} exited -1 "
            f"(TIMED OUT after {TIMEOUT_SECONDS}s, or killed): no "
            f"measurement was obtained, so this cell cannot pin anything")


def _to_json_cell_legs(
    ref_rc: int, ref_stdout: str, ref_json: bytes | None,
    ndrc_rc: int, ndrc_stdout: str, ndrc_json: bytes | None,
) -> tuple[list[str], str]:
    """The three legs every --to-json cell is judged on, in one place.

    Shared verbatim by the matrix sweep (to_json_matrix_check) and the
    flag extras (to_json_extras_check) - the extras are matrix cells with
    a longer argv, so judging them by a second copy of this block would
    only create somewhere for the two to drift apart. The extras' own
    fourth leg (d), the observability guard, has no counterpart in the
    matrix and stays at its call site.

      (a) JSON bytes, after _normalise_clock_symbols is applied to BOTH
          sides - compared when both sides wrote a g.json. A cell where
          only one side wrote one is a failure of its own (the presence
          of the output file is itself observable behaviour), reported
          separately from a byte difference.
      (b) stdout, newline-normalised and with each side's own first-line
          product banner stripped (see _strip_banner_line).
      (c) exit codes.

    A timed-out side fails ahead of all three (_timeout_leg).

    Returns the failure texts (empty when every leg matched) and the
    reference's banner-stripped stdout body, which the sweep needs
    afterwards for its pinned-error tally.
    """
    legs: list[str] = []

    # A timed-out side is an unconditional failure, never a pin - see
    # _timeout_leg.
    timed_out = _timeout_leg(ref_rc, ndrc_rc)
    if timed_out is not None:
        legs.append(timed_out)

    # (a) JSON bytes, clock-normalised on BOTH sides.
    if ref_json is not None and ndrc_json is not None:
        ref_norm = _normalise_clock_symbols(ref_json.decode("latin-1"))
        ndrc_norm = _normalise_clock_symbols(ndrc_json.decode("latin-1"))
        if ref_norm != ndrc_norm:
            legs.append(
                "(a) JSON differs after clock normalisation:\n"
                + format_diff(ref_norm.encode("latin-1"),
                              ndrc_norm.encode("latin-1"),
                              label_a="drf.exe (normalised)",
                              label_b="ndrc (normalised)"))
    elif (ref_json is None) != (ndrc_json is None):
        legs.append(
            f"(a) JSON presence differs: reference "
            f"{'wrote' if ref_json is not None else 'wrote no'} "
            f"g.json, ndrc "
            f"{'wrote' if ndrc_json is not None else 'wrote no'} "
            f"g.json")

    # (b) stdout, banner-stripped on both sides.
    ref_body = _strip_banner_line(_normalise_newlines(ref_stdout))
    ndrc_body = _strip_banner_line(_normalise_newlines(ndrc_stdout))
    if ref_body != ndrc_body:
        legs.append(
            "(b) stdout differs:\n"
            + format_diff(ref_body.encode("utf-8"),
                          ndrc_body.encode("utf-8"),
                          label_a="drf.exe stdout (banner stripped)",
                          label_b="ndrc stdout (banner stripped)"))

    # (c) exit codes.
    if ref_rc != ndrc_rc:
        legs.append(f"(c) exit code differs: reference={ref_rc}, "
                    f"ndrc={ndrc_rc}")

    return legs, ref_body


def _ref_error_text(stdout_body: str) -> str:
    """A reference-failure cell's own error text, for the pinned-error
    tally: the last non-empty line of drf.exe's banner-stripped stdout,
    which is where drf.pas prints its diagnostic before exiting."""
    lines = [ln.strip() for ln in stdout_body.splitlines() if ln.strip()]
    return lines[-1] if lines else "(no stdout)"


def to_json_matrix_check(ndrc: Path, only: list[str] | None = None) -> int:
    """THE MATRIX GATE (spec section 5): the full 910-cell --to-json
    cross-product of TO_JSON_DECK x ALL_TARGET_SUBTARGET_PAIRS x {v2, v3},
    each cell run through reference drf.exe and ndrc --to-json with
    IDENTICAL argv (see _to_json_argv) in identically staged, fresh run
    directories (see _run_to_json_side).

    Three legs per cell - JSON bytes, stdout, exit code - all three
    required to match, and all three implemented in _to_json_cell_legs,
    which the flag extras share.

    EXPECTED-FAILURE SEMANTICS: a cell where the reference itself fails
    (nonzero exit, or exit 0 with no JSON written) is a PIN, never a skip
    - ndrc must reproduce that failure's stdout and exit code exactly,
    and the JSON leg still applies to whatever both sides did write. Such
    a cell is counted as "pin" rather than "pass", and its reference
    error text is tallied, so the sweep reports how much of the matrix is
    pinned failure rather than pinned success. A cell where the reference
    SUCCEEDS and ndrc does not - or where any leg differs either way - is
    a FAIL. No cell is ever excluded. The ONE carve-out from pin
    semantics is a timed-out side, which is a fail on its own and can
    never be pinned (see _timeout_leg).

    Needs the reference toolchain (oracle.local.json) - this is the
    maintainer gate. CI has no drf.exe and instead byte-compares ndrc's
    own --to-json output against the committed fixture JSONs; see
    to_json_goldens_check.

    `only` (repeatable --only) restricts the sweep by cell id
    (to_json_cell_id, AND semantics - see _cell_selected). Prints one
    progress line per fixture, a fixture x pass/pin/fail summary table,
    the distinct reference error texts pinned, and returns nonzero iff
    any selected cell failed.
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    only = only or []
    tally: list[tuple[str, int, int, int]] = []
    failures: list[str] = []
    ref_errors: dict[str, int] = {}
    n_selected = 0

    print(f"to-json matrix: {len(TO_JSON_DECK)} fixtures x "
          f"{len(ALL_TARGET_SUBTARGET_PAIRS)} target/subtarget pairs x 2 "
          f"versions = {TO_JSON_MATRIX_CELLS} cells"
          + (f" (--only {' '.join(only)})" if only else ""))

    for fixture in TO_JSON_DECK:
        dsf = FIXTURES / f"{fixture}.DSF"
        n_cells = n_pass = n_pin = n_fail = 0

        for target, subtarget in ALL_TARGET_SUBTARGET_PAIRS:
            for v3 in (True, False):
                cell = to_json_cell_id(fixture, target, subtarget, v3)
                if not _cell_selected(cell, only):
                    continue
                n_cells += 1
                n_selected += 1

                ref_rc, ref_stdout, ref_json = _run_to_json_side(
                    _to_json_argv(cfg.drf, target, subtarget, v3, ndrc=False),
                    dsf)
                ndrc_rc, ndrc_stdout, ndrc_json = _run_to_json_side(
                    _to_json_argv(ndrc, target, subtarget, v3, ndrc=True),
                    dsf)

                ref_ok = ref_rc == 0 and ref_json is not None
                legs, ref_body = _to_json_cell_legs(
                    ref_rc, ref_stdout, ref_json,
                    ndrc_rc, ndrc_stdout, ndrc_json)

                if legs:
                    n_fail += 1
                    print(f"FAIL {cell}")
                    failures.append(f"{cell}:\n" + "\n".join(legs))
                elif ref_ok:
                    n_pass += 1
                else:
                    n_pin += 1
                    text = _ref_error_text(ref_body)
                    ref_errors[text] = ref_errors.get(text, 0) + 1

        if n_cells:
            tally.append((fixture, n_pass, n_pin, n_fail))
            print(f"  {fixture}: {n_cells} cells, {n_pass} pass, "
                  f"{n_pin} pass-as-pin (reference failure reproduced), "
                  f"{n_fail} fail")

    print(f"\n{'fixture':<10} {'cells':>6} {'pass':>6} {'pin':>6} {'fail':>6}")
    for fixture, n_pass, n_pin, n_fail in tally:
        print(f"{fixture:<10} {n_pass + n_pin + n_fail:>6} {n_pass:>6} "
              f"{n_pin:>6} {n_fail:>6}")
    t_pass = sum(t[1] for t in tally)
    t_pin = sum(t[2] for t in tally)
    t_fail = sum(t[3] for t in tally)
    print(f"{'TOTAL':<10} {n_selected:>6} {t_pass:>6} {t_pin:>6} {t_fail:>6}")

    if ref_errors:
        print(f"\n{len(ref_errors)} distinct reference error text(s) pinned:")
        for text, count in sorted(ref_errors.items(),
                                  key=lambda kv: (-kv[1], kv[0])):
            print(f"  {count:>4}x  {text}")

    for f in failures:
        print(f"\nFAIL {f}")
    return 1 if failures else 0


# The committed --to-json goldens, and the target/subtarget each was
# generated for. These are the cells CI can gate without a reference
# toolchain: ndrc --to-json is run on the staged fixture and its output
# byte-compared against the committed JSON, both clock-normalised (the
# committed files keep whatever YEARHIGH/YEARLOW/MONTH/DAY their own
# generation day wrote - spec section 6(a)). LANG is filename-only: DRF
# takes no language argument, so BLANK_ES's ES lives in the DSF's own
# compression tokens and in the DRB half, never in this invocation.
TO_JSON_GOLDENS: tuple[tuple[str, str, str | None, str], ...] = (
    ("BLANK_EN", "NEXTDAAD", None, "EN"),
    ("BLANK_ES", "NEXTDAAD", None, "ES"),
    ("EXPR", "NEXTDAAD", None, "EN"),
    ("XMSG", "NEXTDAAD", None, "EN"),
    ("INCLUDE", "NEXTDAAD", None, "EN"),
    ("IFDEFS", "NEXTDAAD", None, "EN"),
    ("IFDEFS", "PC", "VGA", "EN"),
    ("IFDEFS", "HTML", None, "EN"),
    # These three were committed as goldens before they were listed here,
    # so Gate 9 ran 8 of the 11 for a while - the drift guard below now
    # makes that state unreachable. The two BLANK_EN rows carry the only
    # non-NEXTDAAD-shaped headers among the goldens (ST's 16-bit layout,
    # ZX/48K's), and EXTERNS is the only golden whose cell needs the
    # EXT_*.BIN sidecar staging to reach a JSON at all.
    ("BLANK_EN", "ST", None, "EN"),
    ("BLANK_EN", "ZX", "48K", "EN"),
    ("EXTERNS", "NEXTDAAD", None, "EN"),
)


def to_json_golden_path(fixture: str, target: str, subtarget: str | None,
                        lang: str) -> Path:
    """The committed golden's path for one TO_JSON_GOLDENS row -
    tests/fixtures/<FIXTURE>.<TARGET>[_<SUB>]_<LANG>_v3.json."""
    slug = target if subtarget is None else f"{target}_{subtarget}"
    return FIXTURES / f"{fixture}.{slug}_{lang}_v3.json"


# The drift guard. TO_JSON_GOLDENS is a hand-written list, so without it
# a newly committed tests/fixtures/*.json can sit in the tree forever
# without Gate 9 ever running it - which is how the three rows above came
# to be committed but ungated. The committed set and the gated set are
# required to be the SAME set, in both directions: a golden added to
# tests/fixtures/ with no row here trips this, and so does a row here
# naming a file that is not committed (which to_json_goldens_check would
# otherwise report only as a per-row FAIL at run time, and only when
# someone runs it).
#
# Import-time, deliberately - the same shape as ndrcoracle.config's own
# ALL_TARGET_SUBTARGET_PAIRS count check, so the mismatch surfaces on ANY
# use of this module rather than only in the one mode that would have
# caught it. An explicit raise rather than `assert`, which python -O
# strips: a gate that silently stops gating under a flag is the very
# failure this guards against.
_GATED_GOLDEN_NAMES = frozenset(
    to_json_golden_path(*row).name for row in TO_JSON_GOLDENS)
_COMMITTED_GOLDEN_NAMES = frozenset(p.name for p in FIXTURES.glob("*.json"))
if _GATED_GOLDEN_NAMES != _COMMITTED_GOLDEN_NAMES:
    raise AssertionError(
        "TO_JSON_GOLDENS has drifted from the committed tests/fixtures/"
        "*.json set - every committed --to-json golden must be gated by "
        "Gate 9 and every gated row must name a committed file. Committed "
        "but NOT gated: "
        f"{sorted(_COMMITTED_GOLDEN_NAMES - _GATED_GOLDEN_NAMES)}; gated "
        f"but NOT committed: "
        f"{sorted(_GATED_GOLDEN_NAMES - _COMMITTED_GOLDEN_NAMES)}")


def to_json_goldens_check(ndrc: Path) -> int:
    """CI leg of the matrix gate: every committed --to-json golden, with
    no reference toolchain involved.

    For each TO_JSON_GOLDENS row: stage the fixture through the SAME
    reference.stage_run_dir helper the maintainer sweep uses (so the
    EXTERNS/INCLUDE/.tok sidecar hooks apply here identically), run
    `ndrc --to-json <TARGET> [SUB] g.DSF g.json -v3`, then byte-compare
    against the committed golden with _normalise_clock_symbols applied to
    BOTH sides (to_json_compare_files, the same single normalisation
    implementation the maintainer gate uses).

    Prints one PASS/FAIL per golden plus a tally, and returns nonzero iff
    any golden failed.
    """
    n_failed = 0
    for fixture, target, subtarget, lang in TO_JSON_GOLDENS:
        golden = to_json_golden_path(fixture, target, subtarget, lang)
        label = to_json_cell_id(fixture, target, subtarget, True)
        if not golden.exists():
            print(f"FAIL {label}: committed golden missing at {golden}")
            n_failed += 1
            continue
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            stage_run_dir(run_dir, FIXTURES / f"{fixture}.DSF")
            argv = _to_json_argv(ndrc, target, subtarget, True, ndrc=True)
            # The sweep sibling (_run_to_json_side) already turns a
            # TimeoutExpired into a reportable outcome; without this the
            # exception escapes and aborts the whole gate with a
            # traceback, losing every golden after the hung one. A
            # timeout is this golden's FAIL and the remaining goldens
            # still run.
            try:
                proc = subprocess.run(argv, cwd=str(run_dir),
                                      capture_output=True, text=True,
                                      errors="replace",
                                      timeout=TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                print(f"FAIL {label}: ndrc TIMED OUT after "
                      f"{TIMEOUT_SECONDS} seconds")
                n_failed += 1
                continue
            out = run_dir / "g.json"
            if proc.returncode != 0:
                print(f"FAIL {label}: ndrc exited {proc.returncode}\n"
                      f"  {proc.stderr.strip() or proc.stdout.strip()}")
                n_failed += 1
                continue
            if not out.exists():
                print(f"FAIL {label}: ndrc --to-json produced no JSON")
                n_failed += 1
                continue
            print(f"golden {label} vs {golden.name}:")
            if to_json_compare_files(out, golden):
                n_failed += 1

    print(f"\n{len(TO_JSON_GOLDENS)} committed --to-json goldens, "
          f"{len(TO_JSON_GOLDENS) - n_failed} passed, {n_failed} failed")
    return 1 if n_failed else 0


# ---------------------------------------------------------------------------
# THE JOIN GOLDEN LEG (Phase 3 task 4) - Gate 10
# ---------------------------------------------------------------------------

def join_goldens_check(ndrc: Path, only: list[str] | None = None) -> int:
    """The join golden leg: every curated_jobs() pair -
    the same 108-pair set the committed goldens come from - driven through
    the join's own single-invocation CLI shape (`ndrc TARGET [SUBTARGET]
    LANG in.DSF [out.ddb] [options]`, main.c's run_join) and byte-compared
    to the committed golden. Needs no reference toolchain at all: this is
    a pure ndrc-vs-committed-bytes gate, which is what lets CI run it as
    Gate 10 with no drf.exe/drb.php/PHP involved.

    Mirrors from_json_check's own (a) comparison - every file a
    multi-file manifest entry names (entry_files), not just "ddb", so an
    -x pair's XMB sidecar is gated too - but drives ndrc through the
    join's DSF-in
    CLI instead of --from-json's two-step JSON handoff, and iterates
    curated_jobs() directly (fixture, combo) rather than reconstructing a
    combo from its slug, so a pair's own flags/v3/classic reach ndrc with
    full fidelity - the same reason from_json_check does it this way (see
    verify_ndrc's CARRY 2 comment for what goes wrong the other way).

    Staging is reference.stage_run_dir, the same DSF-driven helper the
    --to-json matrix and composition leg use, so a fixture's own sidecar
    hooks (EXTERNS's EXT_*.BIN, INCLUDE's INCLUDE2.DSF/INC_DATA.BIN, a
    .tok override) apply here identically.

    No divergence registry here, deliberately: the golden leg's whole
    point is a strict byte gate with no reference toolchain to consult, so
    a difference here is never "authorised" - it is a Task 1-3 bug to fix,
    not a divergence to record.

    `only` filters by "fixture/slug" (AND semantics), same as
    from_json_check's own --only.
    """
    only = only or []
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    n_selected = 0
    n_passed = 0
    failures: list[str] = []

    for fixture, combo in curated_jobs():
        key = f"{fixture}/{combo.slug}"
        if not _pair_selected(fixture, combo.slug, only):
            continue
        n_selected += 1

        entry = manifest.get(key)
        if entry is None:
            print(f"FAIL {key}")
            failures.append(f"{key}: absent from manifest.json")
            continue

        dsf = FIXTURES / f"{fixture}.DSF"
        pair_failures: list[str] = []

        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            stage_run_dir(tmpdir, dsf)

            args = [str(ndrc), combo.target]
            if combo.subtarget:
                args.append(combo.subtarget)
            args += [combo.lang, "g.DSF", "g.ddb"]
            if combo.v3:
                args.append("-v3")
            if combo.classic:
                args.append("-c")
            args.extend(combo.flags)

            # Bounded like every other ndrc spawn here. rc -1 is the
            # synthesised timeout code, never a real ndrc exit, and is
            # an unconditional failure - the same rule
            # to_json_goldens_check applies, so a hung binary fails this
            # pair and the remaining pairs still run instead of the whole
            # gate dying on an escaped exception.
            try:
                proc = subprocess.run(args, cwd=str(tmpdir),
                                      capture_output=True, text=True,
                                      errors="replace",
                                      timeout=TIMEOUT_SECONDS)
                rc = proc.returncode
            except subprocess.TimeoutExpired:
                rc = -1
            out_path = tmpdir / "g.ddb"

            if rc == -1:
                pair_failures.append(
                    f"ndrc exited -1 (TIMED OUT after {TIMEOUT_SECONDS}s, "
                    f"or killed): no measurement was obtained")
            elif rc != 0:
                pair_failures.append(
                    f"ndrc exited {rc}\n  {proc.stderr.strip()}")
            elif not out_path.exists():
                pair_failures.append("ndrc produced no output")
            else:
                produced = out_path.read_bytes()
                ndrc_xmb = collect_xmb_files(tmpdir)

                for name in entry_files(entry):
                    fpath = golden_file_path(fixture, combo.slug, name)
                    if not fpath.exists():
                        pair_failures.append(f"golden missing at {fpath}")
                        continue
                    golden_bytes = fpath.read_bytes()
                    if name == "ddb":
                        if produced != golden_bytes:
                            pair_failures.append(
                                "ddb != golden:\n"
                                + format_diff(golden_bytes, produced,
                                              label_a="golden", label_b="ndrc"))
                    else:
                        ndrc_bytes = ndrc_xmb.get(name)
                        if ndrc_bytes != golden_bytes:
                            pair_failures.append(
                                f"{name} != golden {name}:\n"
                                + format_diff(golden_bytes, ndrc_bytes or b"",
                                              label_a=f"golden {name}",
                                              label_b=f"ndrc {name}"))

        if pair_failures:
            print(f"FAIL {key}")
            failures.append(f"{key}:\n" + "\n".join(pair_failures))
        else:
            print(f"PASS {key}")
            n_passed += 1

    print(f"\n{n_selected} pairs run, {n_passed} passed, "
         f"{len(failures)} failed")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# THE JOIN LIVE DDB MATRIX LEG (Phase 3 task 5)
# ---------------------------------------------------------------------------

# The live leg's grid is the --to-json matrix's, verbatim: the same 13
# fixtures, the same 35 target/subtarget pairs, the same v2/v3 axis = 910
# cells, with the same cell ids (to_json_cell_id) and the same --only
# selection. Aliased rather than re-listed so a fixture added to the deck
# lands in both sweeps at once, and so a cell id names the same cell in
# both.
JOIN_DECK = TO_JSON_DECK
JOIN_MATRIX_CELLS = TO_JSON_MATRIX_CELLS

# Every live cell runs in ENGLISH. The language axis exists on the DRB
# half alone - DRF takes no language argument at all - so doubling the
# grid to 1820 cells would re-run 910 identical front ends to reach it.
# ES token compression IS exercised, by the golden leg's own BLANK_ES
# pairs (gen_goldens.curated_jobs()), so EN-only here is a deliberate
# uniformity choice, not an oversight.
JOIN_MATRIX_LANG = "EN"


def _run_join_reference(cfg, dsf: Path, target: str, subtarget: str | None,
                        v3: bool, drf_extra: tuple[str, ...] = (),
                        drb_extra: tuple[str, ...] = ()
                        ) -> tuple[int, str, bytes | None, dict[str, bytes]]:
    """The reference two-stage flow for one live matrix cell.

    ONE staged run directory (stage_run_dir, the same helper every other
    leg uses), drf.exe then `php drb.php`, because the second stage
    consumes the g.json the first wrote. Both stages take the cell's own
    target/subtarget; only drb takes a language, always JOIN_MATRIX_LANG.

    The flag extras below add `drf_extra`/`drb_extra`: the join hands its
    ONE command line's options to whichever stage owns them, so the
    reference flow has to be given each on that same stage - a drf-side
    option (or the symbol list) on drf, a drb-side one on drb. Both are
    appended last, where the join's own trailing arguments land. The
    matrix passes neither.

    Returns (flow exit, transcript, g.ddb bytes or None, *.XMB sidecars).

    The flow's exit is the FIRST failing stage's exit: a nonzero drf exit
    is the flow's and drb never runs - which is what makes BADSYNTAX's
    cells pin with no drb output on EITHER side - otherwise it is drb's.

    The transcript is the two stages' stdout concatenated with EACH
    stage's own first-line product banner stripped, since ndrc's join
    prints one banner for the whole flow and neither of its stages prints
    one of its own (main.c's run_join). It is returned already newline-
    normalised and BANNER-stripped, the same shape _run_join_ndrc
    returns, so the two go into _join_cell_legs side by side. Only the
    banner line is removed: do NOT add a .strip() here, since
    _strip_stray_error_period matches on a trailing newline and would
    silently stop firing on a transcript whose last one had been eaten.

    A timed-out stage is reported as exit -1 with its own note as the
    transcript, exactly as _run_to_json_side does, so it can never be
    mistaken for a matching failure on the other side (_timeout_leg).
    """
    with tempfile.TemporaryDirectory() as tmp:
        run_dir = Path(tmp)
        stage_run_dir(run_dir, dsf)

        drf_args = [str(cfg.drf), target]
        if subtarget:
            drf_args.append(subtarget)
        drf_args += ["g.DSF", "g.json"]
        if v3:
            drf_args.append("-v3")
        drf_args += list(drf_extra)
        try:
            p1 = subprocess.run(drf_args, cwd=str(run_dir),
                                capture_output=True, text=True,
                                errors="replace", timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return (-1, f"drf TIMED OUT after {TIMEOUT_SECONDS} seconds\n",
                    None, {})

        drf_body = _strip_banner_line(_normalise_newlines(p1.stdout))
        if p1.returncode != 0:
            return p1.returncode, drf_body, None, {}

        drb_args = [str(cfg.php), str(cfg.drb), target]
        if subtarget:
            drb_args.append(subtarget)
        drb_args += [JOIN_MATRIX_LANG, "g.json", "g.ddb"]
        drb_args += list(drb_extra)
        try:
            p2 = subprocess.run(drb_args, cwd=str(run_dir),
                                capture_output=True, text=True,
                                errors="replace", timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return (-1, f"drb TIMED OUT after {TIMEOUT_SECONDS} seconds\n",
                    None, {})

        body = drf_body + _strip_banner_line(_normalise_newlines(p2.stdout))
        out = run_dir / "g.ddb"
        return (p2.returncode, body,
                out.read_bytes() if out.exists() else None,
                collect_xmb_files(run_dir))


def _run_join_ndrc(ndrc: Path, dsf: Path, target: str, subtarget: str | None,
                   v3: bool, extra: tuple[str, ...] = ()
                   ) -> tuple[int, str, bytes | None, dict[str, bytes]]:
    """ndrc's side of one live matrix cell: the join CLI (`ndrc TARGET
    [SUB] EN g.DSF g.ddb [-v3]`, main.c's run_join), in its own freshly
    staged run directory, with the same relative g.DSF/g.ddb names the
    reference flow uses - both sides echo the argument as given, so an
    absolute path on one side alone would diverge the stdout leg on the
    path text rather than on behaviour.

    Returns the same (exit, banner-stripped transcript, g.ddb bytes or
    None, *.XMB sidecars) shape as _run_join_reference. v2 is the -v3
    flag's ABSENCE, as on the reference side.

    `extra` is the flag extras' trailing arguments, all of them on this
    ONE command line whichever stage owns them - which is the whole point
    of the join, and why the reference side needs them split.
    """
    with tempfile.TemporaryDirectory() as tmp:
        run_dir = Path(tmp)
        stage_run_dir(run_dir, dsf)

        args = [str(ndrc), target]
        if subtarget:
            args.append(subtarget)
        args += [JOIN_MATRIX_LANG, "g.DSF", "g.ddb"]
        if v3:
            args.append("-v3")
        args += list(extra)
        try:
            proc = subprocess.run(args, cwd=str(run_dir), capture_output=True,
                                  text=True, errors="replace",
                                  timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return (-1, f"TIMED OUT after {TIMEOUT_SECONDS} seconds\n",
                    None, {})

        out = run_dir / "g.ddb"
        return (proc.returncode,
                _strip_banner_line(_normalise_newlines(proc.stdout)),
                out.read_bytes() if out.exists() else None,
                collect_xmb_files(run_dir))


# drb.php's Error() (drb.php:1351-1355) is `echo("Error: $msg.\n");`, so
# a caller whose own message already ends in a newline gets its period
# and line break emitted AFTER the break the message carried - a stray
# line holding nothing but ".". drb.php:2079 (the 65535-byte boundary
# refusal) is such a caller. NDRC does not reproduce it: diag_fatal's
# fixed "Error: %s.\n" shape cannot, without breaking its own
# no-trailing-period contract, and the owner ruled on 2026-08-26
# (re-affirming the 1a ruling, recorded at backend.c:715-726) that the
# artifact is deliberately not carried over.
#
# The pattern below matches EXACTLY that shape and nothing wider: a line
# consisting of a single "." immediately after a line beginning "Error:
# ". A "." line anywhere else in a transcript is left alone.
_STRAY_ERROR_PERIOD_RE = re.compile(r"^(Error: .*\n)\.\n", re.MULTILINE)


def _strip_stray_error_period(body: str, ref_rc: int) -> str:
    """Removes drb.php's stray lone-"." line from a REFERENCE transcript,
    on refusal cells only (a nonzero flow exit - Error() always exits 2,
    so a zero-exit transcript cannot hold one). ndrc's side is never
    touched. See _STRAY_ERROR_PERIOD_RE for the exact shape and ruling.
    """
    if ref_rc == 0:
        return body
    return _STRAY_ERROR_PERIOD_RE.sub(r"\1", body)


def _join_cell_legs(base_address: int, big_endian: bool,
                    ref_rc: int, ref_body: str, ref_ddb: bytes | None,
                    ndrc_rc: int, ndrc_body: str,
                    ndrc_ddb: bytes | None,
                    ref_xmb: dict[str, bytes] | None = None,
                    ndrc_xmb: dict[str, bytes] | None = None) -> list[str]:
    """The legs every live matrix cell is judged on.

      (a) DDB bytes, compared RAW - a DDB carries no clock stamp (unlike
          the JSON's four clock symbols), so nothing is normalised away
          here. A cell where only one side wrote a DDB is a failure of
          its own, the presence of the output file being observable
          behaviour too - with the refusal carve-out below.
      (b) stdout: the reference flow's two transcripts concatenated
          against ndrc's one, each side already banner-stripped by its
          own runner above, and the reference's already through
          _strip_stray_error_period.
      (c) exit codes: the flow's first-failing-stage exit against ndrc's.
      (XMB) the *.XMB sidecar names and bytes, when both sides' dicts are
          passed - the same leg _xmb_leg gives the flag extras, under the
          same directional carve-out as (a): drb writes sidecars as it
          goes, so a REFERENCE-side extra sidecar on a refusal cell is
          skipped and an ndrc-side extra one still fails.

    A timed-out side fails ahead of all of them (_timeout_leg): a timeout
    is the harness failing to obtain a measurement, never a pin.

    (b) and (c) are decided before (a), because the carve-out (a) may
    take is conditioned on both of them matching; the failure texts are
    still returned in (a), (b), (c) order.

    Returns the failure texts, empty when every leg matched.
    """
    legs: list[str] = []
    ddb_legs: list[str] = []

    timed_out = _timeout_leg(ref_rc, ndrc_rc)
    if timed_out is not None:
        legs.append(timed_out)

    # (b) stdout.
    stdout_leg = None
    if ref_body != ndrc_body:
        stdout_leg = (
            "(b) stdout differs:\n"
            + format_diff(ref_body.encode("utf-8"), ndrc_body.encode("utf-8"),
                          label_a="drf+drb stdout (banners stripped)",
                          label_b="ndrc stdout (banner stripped)"))

    # (c) exit codes.
    exit_leg = None
    if ref_rc != ndrc_rc:
        exit_leg = (f"(c) exit code differs: reference flow={ref_rc}, "
                    f"ndrc={ndrc_rc}")

    # THE REFUSAL PARTIAL-DDB CARVE-OUT (owner-accepted 2026-08-27,
    # narrowed to one direction 2026-08-27).
    # drb.php writes the DDB incrementally into an already-open file, so
    # ANY refusal partway through emission leaves a partial g.ddb on
    # disk; ndrc composes the whole DDB in memory and writes nothing when
    # it refuses. Nothing consumes a partial artifact, so on a cell where
    # the reference flow refuses (nonzero exit) and both sides refuse
    # identically - same stdout, same exit code - a REFERENCE-side extra
    # file is skipped. DIRECTIONAL, deliberately: the accepted mechanism
    # only ever leaves the artifact on the reference side, so a DDB ndrc
    # wrote that the reference did not is still a FAIL - that would be
    # ndrc emitting an output while refusing, which nothing authorises.
    # Everything else about the cell is still compared, and a cell whose
    # stdout or exit differs never reaches this at all.
    # `refusal` is the cell shape the carve-out applies to; each artifact
    # then takes it only in the reference-side-extra direction.
    refusal = ref_rc != 0 and stdout_leg is None and exit_leg is None
    refusal_carve_out = refusal and ref_ddb is not None

    # (a) DDB bytes.
    if ref_ddb is not None and ndrc_ddb is not None:
        if ref_ddb != ndrc_ddb:
            # A refused run can leave a DDB too short to carry the 13
            # header patch words build_section_map reads (last one at
            # offset 32), so section attribution is only attempted on a
            # DDB long enough to hold them.
            section_map = (
                build_section_map(ref_ddb, big_endian=big_endian,
                                  base_address=base_address)
                if len(ref_ddb) >= 34 else None)
            ddb_legs.append(
                "(a) DDB bytes differ:\n"
                + format_diff(ref_ddb, ndrc_ddb, label_a="reference flow",
                              label_b="ndrc join", section_map=section_map))
    elif (ref_ddb is None) != (ndrc_ddb is None) and not refusal_carve_out:
        ddb_legs.append(
            f"(a) DDB presence differs: reference flow "
            f"{'wrote' if ref_ddb is not None else 'wrote no'} g.ddb, "
            f"ndrc {'wrote' if ndrc_ddb is not None else 'wrote no'} "
            f"g.ddb")

    legs.extend(ddb_legs)
    if stdout_leg is not None:
        legs.append(stdout_leg)
    if exit_leg is not None:
        legs.append(exit_leg)

    # (XMB) sidecars, under the same carve-out: on a refusal cell whose
    # stdout and exit already match, a sidecar only the reference wrote
    # is dropped from the comparison, nothing else is.
    if ref_xmb is not None and ndrc_xmb is not None:
        if refusal:
            ref_xmb = {n: b for n, b in ref_xmb.items() if n in ndrc_xmb}
        xmb_leg = _xmb_leg(ref_xmb, ndrc_xmb)
        if xmb_leg is not None:
            legs.append(xmb_leg)

    return legs


def join_matrix_check(ndrc: Path, only: list[str] | None = None) -> int:
    """THE JOIN LIVE LEG: the full 910-cell live DDB
    matrix - JOIN_DECK x ALL_TARGET_SUBTARGET_PAIRS x {v2, v3} - each
    cell built BOTH by the reference two-stage flow (drf.exe then `php
    drb.php`, chained through a g.json on disk - _run_join_reference) and
    by ndrc's own single-invocation join CLI (_run_join_ndrc), then
    judged on four legs: DDB bytes, stdout, exit code and the *.XMB
    sidecars (_join_cell_legs). Default options throughout; the only per-cell arguments are the
    target/subtarget and the version flag, and the language is EN
    everywhere (JOIN_MATRIX_LANG).

    EXPECTED-FAILURE SEMANTICS, the same as the --to-json matrix's: a
    cell where the reference FLOW itself fails (nonzero flow exit, or
    exit 0 with no DDB written) is a PIN, never a skip - ndrc must
    reproduce that failure's stdout and exit code exactly. Both stages
    have their failure path pinned: BADSYNTAX fails at drf (so drb never
    runs and neither side has any drb output at all), while XMSG,
    CONDACTS on the four PC subtargets without XMessage support, and
    BIGDDB on the eight targets whose base address pushes it past 0xFFFF
    all fail at drb (full drf output on both sides, then the identical
    refusal). A cell where the reference flow SUCCEEDS and ndrc does not,
    or where any leg differs either way, is a FAIL. No cell is excluded,
    and a timed-out side is a fail rather than a pin (_timeout_leg).

    Two owner-accepted divergences are normalised out rather than failing
    every refusal cell that meets them: the partial DDB drb.php's
    incremental writing leaves behind (_join_cell_legs' carve-out) and
    the stray lone-"." line its Error() shape emits
    (_strip_stray_error_period). Both are scoped to refusal cells whose
    stdout and exit already match.

    Needs the reference toolchain (oracle.local.json), and PHP: this is
    the maintainer leg. CI's join gate is the goldens leg above
    (join_goldens_check), which needs neither.

    `only` (repeatable --only) restricts the sweep by cell id, the SAME
    ids the --to-json matrix uses (to_json_cell_id, AND semantics), which
    is also how the sweep is sliced per fixture when wall-clock demands.
    Prints one progress line per fixture, a fixture x pass/pin/fail
    summary table, the distinct pinned reference error texts
    (_ref_error_text over the flow's own transcript), and returns nonzero
    iff any selected cell failed.
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    only = only or []
    tally: list[tuple[str, int, int, int]] = []
    failures: list[str] = []
    ref_errors: dict[str, int] = {}
    n_selected = 0

    print(f"join live matrix: {len(JOIN_DECK)} fixtures x "
          f"{len(ALL_TARGET_SUBTARGET_PAIRS)} target/subtarget pairs x 2 "
          f"versions = {JOIN_MATRIX_CELLS} cells"
          + (f" (--only {' '.join(only)})" if only else ""))

    for fixture in JOIN_DECK:
        dsf = FIXTURES / f"{fixture}.DSF"
        n_cells = n_pass = n_pin = n_fail = 0

        for target, subtarget in ALL_TARGET_SUBTARGET_PAIRS:
            for v3 in (True, False):
                cell = to_json_cell_id(fixture, target, subtarget, v3)
                if not _cell_selected(cell, only):
                    continue
                n_cells += 1
                n_selected += 1

                ref_rc, ref_body, ref_ddb, ref_xmb = _run_join_reference(
                    cfg, dsf, target, subtarget, v3)
                ndrc_rc, ndrc_body, ndrc_ddb, ndrc_xmb = _run_join_ndrc(
                    ndrc, dsf, target, subtarget, v3)

                # Applied here rather than inside the leg helper so the
                # pinned-error tally below reads the same normalised
                # transcript the (b) leg compares.
                ref_body = _strip_stray_error_period(ref_body, ref_rc)

                ref_ok = ref_rc == 0 and ref_ddb is not None
                base_address, big_endian = layout_for(target, subtarget)
                legs = _join_cell_legs(
                    base_address, big_endian,
                    ref_rc, ref_body, ref_ddb,
                    ndrc_rc, ndrc_body, ndrc_ddb,
                    ref_xmb, ndrc_xmb)

                if legs:
                    n_fail += 1
                    print(f"FAIL {cell}")
                    failures.append(f"{cell}:\n" + "\n".join(legs))
                elif ref_ok:
                    n_pass += 1
                else:
                    n_pin += 1
                    text = _ref_error_text(ref_body)
                    ref_errors[text] = ref_errors.get(text, 0) + 1

        if n_cells:
            tally.append((fixture, n_pass, n_pin, n_fail))
            print(f"  {fixture}: {n_cells} cells, {n_pass} pass, "
                  f"{n_pin} pass-as-pin (reference failure reproduced), "
                  f"{n_fail} fail")

    print(f"\n{'fixture':<10} {'cells':>6} {'pass':>6} {'pin':>6} {'fail':>6}")
    for fixture, n_pass, n_pin, n_fail in tally:
        print(f"{fixture:<10} {n_pass + n_pin + n_fail:>6} {n_pass:>6} "
              f"{n_pin:>6} {n_fail:>6}")
    t_pass = sum(t[1] for t in tally)
    t_pin = sum(t[2] for t in tally)
    t_fail = sum(t[3] for t in tally)
    print(f"{'TOTAL':<10} {n_selected:>6} {t_pass:>6} {t_pin:>6} {t_fail:>6}")

    if ref_errors:
        print(f"\n{len(ref_errors)} distinct reference error text(s) pinned:")
        for text, count in sorted(ref_errors.items(),
                                  key=lambda kv: (-kv[1], kv[0])):
            print(f"  {count:>4}x  {text}")

    for f in failures:
        print(f"\nFAIL {f}")
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# THE JOIN FLAG EXTRAS (Phase 3 task 6)
# ---------------------------------------------------------------------------

class JoinExtra(NamedTuple):
    """One flag-extra join run: a live matrix cell re-run with extra
    arguments, in DDB space.

    ndrc takes every extra on its ONE command line; the reference flow
    has two, so each extra is recorded on the stage that owns it -
    `drf_extra` (a drf.pas option or the positional symbol list) and
    `drb_extra` (a drb.php option). ndrc's tail is the two concatenated,
    which is exactly what makes these rows a test of the join's own
    routing rather than of either half.

    `changes_ddb`/`changes_stdout`/`changes_xmb` are the leg-(d) record:
    what the extra was MEASURED to do to the REFERENCE flow's own output
    on this cell, checked against a third, BARE reference run rather than
    assumed. Without it an extra can decay into comparing two identical
    no-ops and still pass - a fixture edit that removes the construct the
    flag acts on, or a rename that turns the flag into an argument the
    stage ignores, both leave flagged and bare equal and trip the guard.
    Three fields rather than one because the observable is not in the
    same place for every row: -v moves the transcript alone, -ch/-3h the
    DDB alone, -x the sidecar as well.
    """
    label: str
    fixture: str
    target: str
    subtarget: str | None
    v3: bool
    drf_extra: tuple[str, ...]
    drb_extra: tuple[str, ...]
    changes_ddb: bool
    changes_stdout: bool
    changes_xmb: bool


# One row per option that changes what the JOIN emits, less the two
# semantic flags and -replace-xcondacts: those are unobservable on any
# committed fixture (every fixture compiles clean, and no fixture uses
# XPICTURE/XSAVE/XLOAD/XBEEP), so they stay in test_cli.py against
# variants generated at test runtime - the join's own -replace-xcondacts
# routing among them.
#
# Every observable below was MEASURED against the reference flow (drf.exe
# 0.40 then drb.php) on 2026-08-27 before being written here, and the
# leg-(d) triple is that measurement. Labels lead with the fixture name so
# `--only <FIXTURE>` selects a fixture's extras alongside its matrix cells.
JOIN_EXTRA_JOBS: tuple[JoinExtra, ...] = (
    # -7 (drf stage): 2017 -> 2010 DDB bytes as the accented text folds
    # (System Mesages 844 -> 839, Location texts 112 -> 110, compression
    # savings 159 -> 165), and the unconditional "Generating DAAD 7-bit
    # ASCII DDB" line joins the transcript at the drf stage's position.
    JoinExtra("join_x_BLANK_ES_ascii7", "BLANK_ES", "NEXTDAAD", None, True,
              ("-7",), (), True, True, False),

    # -check-maluva-disabled (drf stage): a complete NO-OP in DDB space -
    # 2086 bytes either way, byte-identical, identical transcript, no
    # sidecar either way. The flag's only effect is the JSON's own
    # maluva_used field (UJSONExport.pas:291), which no drb consumer
    # reads back into the DDB, so leg (d) pins the no-op (all three
    # False) instead of a difference. The flip itself is gated in JSON
    # space by 2b's tojson_x_EXTERNS_check_maluva_disabled row.
    JoinExtra("join_x_EXTERNS_check_maluva_disabled", "EXTERNS", "NEXTDAAD",
              None, True, ("-check-maluva-disabled",), (), False, False,
              False),

    # -force-normal-messages (drf stage): XMESSAGE demoted to MESSAGE, so
    # the message tables grow (7994 -> 8027 bytes, User Messages 2710 ->
    # 2726) and the bare run's 40-byte 0.XMB DISAPPEARS - the xmessage
    # table is empty, so drb writes no sidecar and drops its "XMessages
    # size is 40 bytes in files of 64K." line. CONDACTS, not XMSG: XMSG's
    # XDATA refusal fires regardless of message forcing, so it never
    # yields a DDB to compare.
    JoinExtra("join_x_CONDACTS_force_normal_messages", "CONDACTS", "NEXTDAAD",
              None, True, ("-force-normal-messages",), (), True, True, True),

    # -force-x-messages (drf stage): the opposite migration - 7994 ->
    # 5236 DDB bytes as the message text moves out, and 0.XMB goes from
    # 40 to 2660 bytes.
    JoinExtra("join_x_CONDACTS_force_x_messages", "CONDACTS", "NEXTDAAD",
              None, True, ("-force-x-messages",), (), True, True, True),

    # The positional SYMBOL LIST (drf stage): "CLIONLY" compiles
    # IFDEFS.DSF:475-478's `#ifdef "CLIONLY"` block, so the process bytes
    # gain a SET 17 entry (2f 11 at DDB offset 0x74A - condact 47,
    # UCondacts.pas:73) and the DDB grows 2062 -> 2069. The bare side of
    # the flip is the matrix's own IFDEFS_NEXTDAAD_v3 cell; leg (d) is
    # what ties the two together.
    JoinExtra("join_x_IFDEFS_symbols", "IFDEFS", "NEXTDAAD", None, True,
              ("CLIONLY",), (), True, True, False),

    # -v (drb stage): the DDB is byte-identical (2038 both ways) - the
    # whole observable is the drb stage's verbose block, at the drb
    # stage's own transcript position: "Verbose mode on", the section
    # addresses, the unused-token warnings, the adventure totals.
    JoinExtra("join_x_BLANK_EN_verbose", "BLANK_EN", "NEXTDAAD", None, True,
              (), ("-v",), False, True, False),

    # -c (drb stage), on ZX/48K per CONTROLLER RULING 3, NOT NEXTDAAD:
    # ndrc refuses classic on NEXTDAAD by design (backend.c's 1b ruling)
    # while the reference flow builds it, so a NEXTDAAD -c cell is a
    # known divergence rather than a gate candidate. Here: 2038 -> 2144
    # bytes, dedup off and tokens padded, with the block sizes shifting
    # in the transcript to match.
    JoinExtra("join_x_BLANK_EN_classic", "BLANK_EN", "ZX", "48K", True,
              (), ("-c",), True, True, False),

    # -p (drb stage): padding bytes, 2038 -> 2112, every padded section's
    # size line moving with them.
    JoinExtra("join_x_BLANK_EN_padding", "BLANK_EN", "NEXTDAAD", None, True,
              (), ("-p",), True, True, False),

    # -x (drb stage): the message and location text moves out of the DDB
    # (2038 -> 1940, User Messages 3 -> 2, Location texts 103 -> 6) into
    # a 98-byte 0.XMB the bare run never writes. The sidecar BYTES are
    # compared on both sides, not just its presence - see the XMB leg in
    # join_extras_check.
    JoinExtra("join_x_BLANK_EN_xmb_dump", "BLANK_EN", "NEXTDAAD", None, True,
              (), ("-x",), True, True, True),

    # -ch (drb stage), on C64 - the one target the flag is valid for:
    # a 2-byte load-address prefix, 2038 -> 2040, the first two bytes
    # becoming the load address. The transcript does NOT change: drb
    # reports the DDB's own size, not the prefixed file's.
    JoinExtra("join_x_BLANK_EN_c64_header", "BLANK_EN", "C64", None, True,
              (), ("-ch",), True, False, False),

    # -3h (drb stage), on ZX/48K: the 128-byte +3DOS header, 2038 ->
    # 2166. Silent in the transcript for the same reason as -ch.
    JoinExtra("join_x_BLANK_EN_plus3_header", "BLANK_EN", "ZX", "48K", True,
              (), ("-3h",), True, False, False),

    # -b=0x9000 (drb stage): the base-address shift - same 2038 bytes,
    # every absolute address the DDB carries moved up by 0x9000, and the
    # transcript's "Database starts at 0 (0x0000)"/"ends at address 2038
    # (0x07F6)" pair becoming 36864 (0x9000)/38902 (0x97F6).
    JoinExtra("join_x_BLANK_EN_base_address", "BLANK_EN", "NEXTDAAD", None,
              True, (), ("-b=0x9000",), True, True, False),
)


def _xmb_leg(ref: dict[str, bytes],
             produced: dict[str, bytes]) -> str | None:
    """The reference flow's *.XMB sidecars against ndrc's, or None when
    the two match.

    Names first (a sidecar written on one side alone is observable
    behaviour of its own), then the bytes of every name they share.
    """
    if ref == produced:
        return None
    if sorted(ref) != sorted(produced):
        return (f"(XMB) sidecars differ: reference flow wrote "
                f"{sorted(ref) or 'none'}, ndrc wrote "
                f"{sorted(produced) or 'none'}")
    parts = ["(XMB) sidecar bytes differ:"]
    for name in sorted(ref):
        if ref[name] != produced[name]:
            parts.append(f"{name}:\n"
                         + format_diff(ref[name], produced[name],
                                       label_a="reference flow",
                                       label_b="ndrc join"))
    return "\n".join(parts)


def join_extras_check(ndrc: Path, only: list[str] | None = None) -> int:
    """The join flag extras, appended after the 910-cell live matrix
    sweep with their own summary line.

    Each row of JOIN_EXTRA_JOBS re-runs one live matrix cell with extra
    arguments - ndrc getting all of them on its one command line, the
    reference flow getting each on the stage that owns it - through the
    matrix's own legs, the SAME _join_cell_legs the sweep calls, so the
    two can never drift and these rows inherit its carve-outs (the
    directional partial-DDB skip, the lone-"." strip):

      (a) DDB bytes, raw.
      (b) stdout, banner-stripped on both sides.
      (c) exit codes.
      (XMB) the *.XMB sidecars, names and bytes - the -x row's whole
          point, and free for the rest.

    Plus one of its own:

      (d) the OBSERVABILITY guard: a third, BARE reference run of the
          same cell, whose DDB, transcript and sidecars must differ from
          the flagged reference's exactly where the row's own
          changes_ddb/changes_stdout/changes_xmb say they do. See
          JoinExtra's docstring.

    A timed-out side fails the row outright rather than pinning it, the
    same rule the matrix uses (_timeout_leg).

    Selection: `only`'s substrings must all appear in the row's label
    (AND semantics, as in to_json_extras_check), and every label leads
    with its fixture name so a `--only <FIXTURE>` slice of the matrix
    drags that fixture's extras along with it.
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    only = only or []
    n_selected = 0
    n_passed = 0
    failures: list[str] = []

    for job in JOIN_EXTRA_JOBS:
        if not all(substr in job.label for substr in only):
            continue
        n_selected += 1

        dsf = FIXTURES / f"{job.fixture}.DSF"
        ref_rc, ref_body, ref_ddb, ref_xmb = _run_join_reference(
            cfg, dsf, job.target, job.subtarget, job.v3,
            job.drf_extra, job.drb_extra)
        ndrc_rc, ndrc_body, ndrc_ddb, ndrc_xmb = _run_join_ndrc(
            ndrc, dsf, job.target, job.subtarget, job.v3,
            job.drf_extra + job.drb_extra)
        bare_rc, bare_body, bare_ddb, bare_xmb = _run_join_reference(
            cfg, dsf, job.target, job.subtarget, job.v3)

        ref_body = _strip_stray_error_period(ref_body, ref_rc)
        bare_body = _strip_stray_error_period(bare_body, bare_rc)

        # -b= moves the addresses build_section_map attributes a failure
        # diff by, so the row's own drb flags decide the base address the
        # same way the --from-json sweep's do.
        base_address, big_endian = layout_for(
            job.target, job.subtarget,
            base_override=_base_override_from_flags(job.drb_extra))
        legs = _join_cell_legs(base_address, big_endian,
                               ref_rc, ref_body, ref_ddb,
                               ndrc_rc, ndrc_body, ndrc_ddb,
                               ref_xmb, ndrc_xmb)

        flag_text = " ".join(job.drf_extra + job.drb_extra)
        if bare_rc == -1:
            legs.append("(d) the bare reference flow timed out, so this "
                        "row's observability could not be checked")
        elif ref_ddb is None or bare_ddb is None:
            legs.append(
                f"(d) observability unknown: the reference flow wrote "
                f"{'a' if ref_ddb is not None else 'no'} g.ddb with "
                f"`{flag_text}` and "
                f"{'a' if bare_ddb is not None else 'no'} g.ddb without it "
                f"- every extra here was measured to write one both ways")
        else:
            for what, differs, expected in (
                    ("DDB bytes", ref_ddb != bare_ddb, job.changes_ddb),
                    ("stdout", ref_body != bare_body, job.changes_stdout),
                    ("XMB sidecars", ref_xmb != bare_xmb, job.changes_xmb)):
                if differs != expected:
                    now = "changes them" if differs else "leaves them alone"
                    legs.append(
                        f"(d) observability: `{flag_text}` was measured to "
                        f"{'CHANGE' if expected else 'LEAVE UNCHANGED'} the "
                        f"reference flow's {what} on this cell, but it now "
                        f"{now} - either the reference behaviour moved or "
                        f"this cell no longer exercises the flag")

        if legs:
            print(f"FAIL {job.label}")
            failures.append(f"{job.label}:\n" + "\n".join(legs))
        else:
            print(f"PASS {job.label}")
            n_passed += 1

    print(f"\n{n_selected} join flag extras run, {n_passed} passed, "
          f"{len(failures)} failed")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# THE --to-json FLAG EXTRAS
# ---------------------------------------------------------------------------

class ToJsonExtra(NamedTuple):
    """One flag-extra --to-json run: a matrix cell re-run with extra
    trailing arguments both sides get identically.

    `changes_json` records what the flag was MEASURED to do to the
    reference's own output on THIS cell, and is checked rather than
    assumed: a third, BARE reference run of the same cell is compared
    against the flagged one, and the two must differ exactly when this
    says so. That is what stops an extra from quietly ceasing to exercise
    its flag - a fixture edit that removes the construct the flag acts
    on, or a rename that turns the flag into an argument DRF ignores,
    both leave the flagged and bare outputs equal and trip the guard
    instead of passing on a comparison of two identical no-ops.
    """
    label: str
    fixture: str
    target: str
    subtarget: str | None
    v3: bool
    extra_args: tuple[str, ...]
    changes_json: bool


# One row per DRF option that changes what --to-json emits, less the two
# semantic flags (-no-semantic / -semantic-warnings): those cannot be
# observable on any committed fixture, since every fixture compiles clean
# and semantic analysis therefore has nothing to reject, so they run in
# test_cli.py against a deliberately-invalid variant generated at test
# runtime.
#
# Every observable below was MEASURED against reference drf.exe 0.40 on
# 2026-08-27 before being written here, and each is stated with the
# Pascal line that produces it. The label leads with the fixture name so
# `--only <FIXTURE>` selects a fixture's extras alongside its matrix
# cells.
TO_JSON_EXTRA_JOBS: tuple[ToJsonExtra, ...] = (
    # -7 (drf.pas:399-403 -> ASCII7 -> UJSONExport.pas:399's
    # ConvertAscii7Chars): every accented byte in the string tables folds
    # to its unaccented 7-bit letter. MEASURED on BLANK_ES/NEXTDAAD/-v3:
    # 15 "Text" lines change. Quoted here in the JSON's own \u escapes,
    # because the accented source bytes are cp1252 and this file is not:
    # "Est\u0015 demasiado oscuro para ver nada." -> "Esta demasiado
    # oscuro para ver nada.", and "\u0012Seguro?" -> "#Seguro?" - the
    # inverted-question-mark slot folds to '#', not to nothing.
    # -7 ALSO prints its own "Generating DAAD 7-bit ASCII DDB"
    # line UNCONDITIONALLY (not gated on -verbose, unlike most of the
    # flags below), immediately after "Generating DAAD V3 DDB" and BEFORE
    # "Reading g.DSF" - it is printed from the option loop, not from the
    # compile - so leg (b) pins that line's position too.
    ToJsonExtra("tojson_x_BLANK_ES_ascii7", "BLANK_ES", "NEXTDAAD", None,
                True, ("-7",), True),

    # -force-normal-messages (drf.pas:373-377 -> ForceNormalMessages ->
    # USintactic.pas:629): the XMES/XMESSAGE arm is skipped, so the
    # CASE at USintactic.pas:640-643 demotes the opcode instead.
    # MEASURED on XMSG/NEXTDAAD/-v3: the fixture's one `XMES "xmes direct
    # text"` becomes {"Opcode":77,"Condact":"MES","Param1":1} and its text
    # MOVES OUT of the xmessages table (where it sat at Value 0) INTO the
    # message table at Value 1 - the mtx grows by one entry, exactly as
    # the brief predicted.
    ToJsonExtra("tojson_x_XMSG_force_normal_messages", "XMSG", "NEXTDAAD",
                None, True, ("-force-normal-messages",), True),

    # -force-x-messages (drf.pas:379-383 -> ForceXMessages ->
    # USintactic.pas:623-627): the opposite rewrite, MES -> XMES and
    # MESSAGE -> XMESSAGE, before the XMES arm then pools the text.
    # MEASURED on CONDACTS/NEXTDAAD/-v3: a ~4500-line JSON difference -
    # every message-carrying condact in the deck's largest fixture
    # migrates, which shifts the whole "sysmess"/"messages"/"xmessages"
    # block structure, not just the condact opcodes.
    ToJsonExtra("tojson_x_CONDACTS_force_x_messages", "CONDACTS", "NEXTDAAD",
                None, True, ("-force-x-messages",), True),

    # -check-maluva-disabled (drf.pas:385-389 -> CheckMaluva := false).
    # Despite the name it performs NO check: CheckMaluva's only reader is
    # UJSONExport.pas:291's `byte(MaluvaUsed OR NOT CheckMaluva)`, so
    # disabling it forces the emitted maluva_used field high. MEASURED on
    # EXTERNS/NEXTDAAD/-v3: a ONE-LINE difference, {"classic_mode":0,
    # "debug_mode":0, "v3code":1, "maluva_used":0} -> the same line with
    # "maluva_used":1, exit 0 both ways with identical stdout. NOT an
    # error run, which is what the brief left open.
    ToJsonExtra("tojson_x_EXTERNS_check_maluva_disabled", "EXTERNS",
                "NEXTDAAD", None, True, ("-check-maluva-disabled",), True),

    # -replace-xcondacts (drf.pas:405-409 -> replace_xcondacts ->
    # USintactic.pas:583-602). MEASURED on CONDACTS/NEXTDAAD/-v3: a
    # complete NO-OP - byte-identical JSON, identical stdout, exit 0 -
    # which CONTRADICTS the brief's "X-condact rewrite" expectation, so
    # the measurement stands and this row pins the no-op (changes_json
    # False) rather than a rewrite.
    #
    # The reason is structural, not fixture luck: the guarded block
    # touches only XPICTURE, XSAVE, XLOAD and XBEEP, and no committed
    # fixture uses any of the four (checked across all 14 .DSF files).
    # Nor does it "replace" anything - three of the four arms are bare
    # SyntaxErrors ("XSAVE has been deprecated, use SAVE instead.") and
    # the fourth sets MaluvaUsed on four 8-bit targets and errors
    # everywhere else. Confirmed reachable by a scratch probe: BLANK_EN
    # plus one `XPICTURE 0` compiles clean WITHOUT the flag and fails
    # WITH it - "446:24:g.dsf: XPICTURE cannot be used in this target
    # [NEXTDAAD].." (the doubled period is the reference's own). So what
    # this row pins is that ndrc accepts the option and, like the
    # reference, changes nothing on a deck that never reaches it.
    ToJsonExtra("tojson_x_CONDACTS_replace_xcondacts", "CONDACTS",
                "NEXTDAAD", None, True, ("-replace-xcondacts",), False),

    # The positional SYMBOL LIST (drf.pas:354 -> AdditionalSymbols ->
    # drf.pas:292-300's ExtractWord loop). MEASURED on IFDEFS/NEXTDAAD/
    # -v3 with "CLIONLY,SECOND": the JSON gains {"symbol":"SECOND",
    # "Value":2} and {"symbol":"CLIONLY", "Value":1}, and process 7's SET
    # entries go from {10, 11, 14} to {10, 11, 14, 17} as the fixture's
    # `#ifdef "CLIONLY"` block (IFDEFS.DSF:475-478) starts compiling.
    # SECOND names nothing in the fixture and is there purely to pin that
    # the SECOND slot takes value 2. The bare side of the flip is the
    # matrix's own IFDEFS_NEXTDAAD_v3 cell; changes_json True is what
    # ties the two together here, and test_cli.py pins the slot and
    # separator semantics (commas only, empty slots collapsed) exactly.
    ToJsonExtra("tojson_x_IFDEFS_symbols", "IFDEFS", "NEXTDAAD", None,
                True, ("CLIONLY,SECOND",), True),
)


def to_json_extras_check(ndrc: Path, only: list[str] | None = None) -> int:
    """The --to-json flag extras, appended after the 910-cell matrix
    sweep with their own summary line.

    Each row of TO_JSON_EXTRA_JOBS re-runs one matrix cell with extra
    trailing arguments, given IDENTICALLY to both sides (_to_json_argv's
    `extra`), in identically staged fresh run directories
    (_run_to_json_side), through the matrix's own three legs - the SAME
    _to_json_cell_legs the sweep calls, so the two can never drift:

      (a) JSON bytes, clock-normalised on both sides.
      (b) stdout, newline-normalised and banner-stripped on both sides.
      (c) exit codes.

    Plus the OBSERVABILITY guard: a third, BARE reference run of the same
    cell, whose JSON must differ from the flagged reference JSON exactly
    when the row's own `changes_json` says it does. Without it an extra
    can decay into comparing two identical no-ops and still pass - see
    ToJsonExtra's docstring.

    A timed-out side fails the row outright rather than pinning it, the
    same rule the matrix uses (_timeout_leg).

    Selection: `only`'s substrings must all appear in the row's label
    (AND semantics, as in flags_extra_check), and every label leads with
    its fixture name so a `--only <FIXTURE>` slice of the matrix drags
    that fixture's extras along with it.
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    only = only or []
    n_selected = 0
    n_passed = 0
    failures: list[str] = []

    for job in TO_JSON_EXTRA_JOBS:
        if not all(substr in job.label for substr in only):
            continue
        n_selected += 1

        dsf = FIXTURES / f"{job.fixture}.DSF"
        ref_rc, ref_stdout, ref_json = _run_to_json_side(
            _to_json_argv(cfg.drf, job.target, job.subtarget, job.v3,
                          ndrc=False, extra=job.extra_args), dsf)
        ndrc_rc, ndrc_stdout, ndrc_json = _run_to_json_side(
            _to_json_argv(ndrc, job.target, job.subtarget, job.v3,
                          ndrc=True, extra=job.extra_args), dsf)
        bare_rc, _bare_stdout, bare_json = _run_to_json_side(
            _to_json_argv(cfg.drf, job.target, job.subtarget, job.v3,
                          ndrc=False), dsf)

        # (a), (b), (c) - the matrix's own legs, from the matrix's own
        # implementation.
        legs, _ref_body = _to_json_cell_legs(
            ref_rc, ref_stdout, ref_json, ndrc_rc, ndrc_stdout, ndrc_json)

        # (d) the observability guard, on the REFERENCE alone: does the
        # extra still do to drf.exe what it was measured to do here?
        if bare_rc == -1:
            legs.append(
                "(d) the bare reference run timed out, so this row's "
                "observability could not be checked")
        elif ref_json is None or bare_json is None:
            legs.append(
                f"(d) observability unknown: reference wrote "
                f"{'a' if ref_json is not None else 'no'} g.json with "
                f"{' '.join(job.extra_args)} and "
                f"{'a' if bare_json is not None else 'no'} g.json without it "
                f"- every extra here was measured to write one both ways")
        else:
            differs = (_normalise_clock_symbols(ref_json.decode("latin-1"))
                       != _normalise_clock_symbols(
                           bare_json.decode("latin-1")))
            if differs != job.changes_json:
                legs.append(
                    f"(d) observability: `{' '.join(job.extra_args)}` was "
                    f"measured to "
                    f"{'CHANGE' if job.changes_json else 'LEAVE UNCHANGED'} "
                    f"the reference JSON on this cell, but it now "
                    f"{'changes' if differs else 'leaves unchanged'} it - "
                    f"either the reference behaviour moved or this cell no "
                    f"longer exercises the flag")

        if legs:
            print(f"FAIL {job.label}")
            failures.append(f"{job.label}:\n" + "\n".join(legs))
        else:
            print(f"PASS {job.label}")
            n_passed += 1

    print(f"\n{n_selected} --to-json flag extras run, {n_passed} passed, "
          f"{len(failures)} failed")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


# task-7-brief.md Step 5 / task-6-brief.md Step 5: fixed flag-extra pairs,
# outside the curated matrix (no committed golden - none of these flags are
# swept by gen_goldens.py, except -d/-p which get real goldens too - Set H/I
# - but those don't exercise the RETARGET/TRUNCATION effect this mechanism
# checks). The 5-tuple's own compare_stdout flag (task-6-brief.md Step 5)
# extends the original bytes-only check: True routes the pair through the
# stdout (and exit-code) comparison added below, for pairs whose only
# observable effect isn't in the DDB bytes at all (flags_d_st's retarget
# echo) or where the bytes alone are a weak signal on their own (flags_np_*'s
# truncated-and-terminated DDB).
#   flags_c64ch/flags_zx3h (task-7): C64 for -ch, ZX/PLUS3 for -3h - the one
#     target each flag is valid for (main.c's own -ch/-3h target guard).
#   flags_d_st (task-6): BLANK_EN/ST/-d - BLANK_EN carries no DEBUG condact,
#     so -d changes no DDB byte at all here; the retarget echo ("Debug mode
#     active, but target is not ZX...") is the only observable effect,
#     which is why this needs compare_stdout - deliberately NOT the DEBUG
#     fixture (Set H already covers -d's downstream hashing/emission
#     consequence there).
#   flags_np_nextdaad/flags_np_pc (task-6): BLANK_EN/-np, on a non-padding
#     target (NEXTDAAD) and a padding one (PC/VGA256) - drb.php:290's
#     `exit;` fires unconditionally at the FIRST addPaddingIfRequired call
#     regardless of either target's own padding-platform-ness (measured,
#     task-6-report.md), truncating both to the same 60-byte header+extvec
#     DDB with exit code 0 and no further stdout.
#   flags_b_hex/flags_b_dec (task-6): ZX/48K/-b=0x9000 and -b=36864 (the
#     same value, hex vs decimal) - bytes-only suffices since -b= visibly
#     shifts every absolute address the DDB carries.
FLAG_EXTRA_JOBS: list[tuple[str, str, Combo, str, bool]] = [
    ("flags_c64ch", "BLANK_EN",
     Combo(target="C64", subtarget=None, lang="EN", v3=True, classic=False),
     "-ch", False),
    ("flags_zx3h", "BLANK_EN",
     Combo(target="ZX", subtarget="PLUS3", lang="EN", v3=True, classic=False),
     "-3h", False),
    ("flags_d_st", "BLANK_EN",
     Combo(target="ST", subtarget=None, lang="EN", v3=True, classic=False),
     "-d", True),
    ("flags_np_nextdaad", "BLANK_EN",
     Combo(target="NEXTDAAD", subtarget=None, lang="EN", v3=True, classic=False),
     "-np", True),
    ("flags_np_pc", "BLANK_EN",
     Combo(target="PC", subtarget="VGA256", lang="EN", v3=True, classic=False),
     "-np", True),
    ("flags_b_hex", "BLANK_EN",
     Combo(target="ZX", subtarget="48K", lang="EN", v3=True, classic=False),
     "-b=0x9000", False),
    ("flags_b_dec", "BLANK_EN",
     Combo(target="ZX", subtarget="48K", lang="EN", v3=True, classic=False),
     "-b=36864", False),
]


def flags_extra_check(ndrc: Path, only: list[str] | None = None) -> int:
    """The fixed flag-extra pairs (task-7-brief.md Step 5, extended by
    task-6-brief.md Step 5).

    Unlike from_json_check's curated_jobs() sweep, these are not part of
    the committed golden matrix - no gen_goldens.py job builds a -ch/-3h/
    -np/... DDB with these exact flags - so there is nothing to compare
    ndrc's output against but a FRESH reference run built with the
    identical extra flag, fresh-vs-fresh only. Runs DRF then DRB directly
    (rather than through reference.run_reference_from_json, which has no
    extra-flags parameter) since only these fixed pairs ever need one.

    Every pair compares produced DDB bytes. A pair whose own compare_stdout
    is True (see FLAG_EXTRA_JOBS above) additionally compares DRB's own
    stdout against ndrc's, each newline-normalised and banner-stripped
    exactly as from_json_check's own (c) check does, plus an explicit
    exit-code equality check - both are already required to independently
    be 0 by the unconditional-failure branches below, so this is belt and
    braces for a flag whose whole point is an early, deliberate exit
    (flags_np_*), not a new avenue by which a nonzero exit could pass.

    Selection: a job runs when `only` is empty, or every substring in
    `only` appears in the job's own label (e.g. "flags_c64ch") - same AND
    semantics as _pair_selected, against the label instead of
    "fixture/slug" since these pairs have no golden-matrix key.
    """
    try:
        cfg = load_config()
    except ConfigError as e:
        print(f"ERROR: {e}")
        return 2

    only = only or []
    n_selected = 0
    n_passed = 0
    failures: list[str] = []

    for label, fixture, combo, flag, compare_stdout in FLAG_EXTRA_JOBS:
        if not all(substr in label for substr in only):
            continue
        n_selected += 1

        dsf = FIXTURES / f"{fixture}.DSF"
        run_dir = Path(tempfile.mkdtemp(
            prefix=f"flagref_{label}_", dir=str(cfg.workdir)))
        ref_ok = True
        ref_ddb: bytes | None = None
        ref_drb_stdout = ""
        ref_exit_code = 0
        json_bytes: bytes | None = None
        try:
            shutil.copyfile(dsf, run_dir / "g.DSF")

            drf_args = [str(cfg.drf), *combo.drf_args, "g.DSF", "g.json"]
            if combo.v3:
                drf_args.append("-v3")
            p1 = subprocess.run(drf_args, cwd=str(run_dir),
                                capture_output=True, text=True,
                                errors="replace")
            if p1.returncode != 0:
                failures.append(f"{label}: reference DRF exited {p1.returncode}\n"
                                f"  {p1.stderr.strip() or p1.stdout.strip()}")
                ref_ok = False
            else:
                json_path = run_dir / "g.json"
                if not json_path.exists():
                    failures.append(f"{label}: reference DRF produced no JSON")
                    ref_ok = False
                else:
                    json_bytes = json_path.read_bytes()

                    drb_args = [str(cfg.php), str(cfg.drb), *combo.drb_args,
                               "g.json", "g.DDB", flag]
                    if combo.classic:
                        drb_args.append("-c")
                    p2 = subprocess.run(drb_args, cwd=str(run_dir),
                                        capture_output=True, text=True,
                                        errors="replace")
                    ref_drb_stdout = p2.stdout
                    ref_exit_code = p2.returncode
                    if p2.returncode != 0:
                        failures.append(
                            f"{label}: reference DRB exited {p2.returncode}\n"
                            f"  {p2.stderr.strip() or p2.stdout.strip()}")
                        ref_ok = False
                    else:
                        ddb_path = run_dir / "g.DDB"
                        if not ddb_path.exists():
                            failures.append(
                                f"{label}: reference DRB produced no DDB")
                            ref_ok = False
                        else:
                            ref_ddb = ddb_path.read_bytes()
        finally:
            shutil.rmtree(run_dir, ignore_errors=True)

        if not ref_ok:
            print(f"FAIL {label}")
            continue

        ndrc_args = [str(ndrc), "--from-json", combo.target]
        if combo.subtarget:
            ndrc_args.append(combo.subtarget)
        ndrc_args += [combo.lang, "g.json", "g.DDB", flag]
        if combo.classic:
            ndrc_args.append("-c")

        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            _stage_ndrc_run_dir(tmpdir, json_bytes, fixture)
            proc = subprocess.run(ndrc_args, cwd=str(tmpdir),
                                  capture_output=True, text=True,
                                  errors="replace")
            out_path = tmpdir / "g.DDB"
            # flags_np_* deliberately exit 0 with a truncated (not missing)
            # DDB - same shape as the reference DRB run above, so a nonzero
            # exit or a missing file is still an unconditional failure for
            # every job here, compare_stdout included.
            if proc.returncode != 0:
                failures.append(f"{label}: ndrc exited {proc.returncode}\n"
                                f"  {proc.stderr.strip()}")
                print(f"FAIL {label}")
                continue
            if not out_path.exists():
                failures.append(f"{label}: ndrc produced no output")
                print(f"FAIL {label}")
                continue
            produced = out_path.read_bytes()
            ndrc_stdout = proc.stdout
            ndrc_exit_code = proc.returncode

        pair_failures: list[str] = []
        if produced != ref_ddb:
            pair_failures.append(
                f"ndrc != fresh reference (bytes):\n"
                + format_diff(ref_ddb, produced,
                              label_a="fresh reference", label_b="ndrc"))

        if compare_stdout:
            # Same comparison shape as from_json_check's (c): newline-
            # normalised, each side's own first-line product banner
            # stripped, since neither has a counterpart on the other side.
            ndrc_body = _strip_banner_line(_normalise_newlines(ndrc_stdout))
            ref_body = _strip_banner_line(_normalise_newlines(ref_drb_stdout))
            if ndrc_body != ref_body:
                pair_failures.append(
                    "stdout differs:\n"
                    + format_diff(ref_body.encode("utf-8"),
                                  ndrc_body.encode("utf-8"),
                                  label_a="drb stdout (banner stripped)",
                                  label_b="ndrc stdout (banner stripped)"))
            if ndrc_exit_code != ref_exit_code:
                pair_failures.append(
                    f"exit code differs: reference={ref_exit_code}, "
                    f"ndrc={ndrc_exit_code}")

        if pair_failures:
            failures.append(f"{label}:\n" + "\n".join(pair_failures))
            print(f"FAIL {label}")
        else:
            print(f"PASS {label}")
            n_passed += 1

    print(f"\n{n_selected} flag-extra pairs run, {n_passed} passed, "
         f"{len(failures)} failed")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ndrc", type=Path,
                    help="path to the ndrc binary to verify")
    ap.add_argument("--self-check", action="store_true",
                    help="verify goldens against their manifest only")
    ap.add_argument("--from-json", action="store_true",
                    help="maintainer gate: rebuild curated_jobs() pairs "
                         "through ndrc --from-json against fresh reference "
                         "runs; needs the reference toolchain "
                         "(oracle.local.json) and --ndrc")
    ap.add_argument("--to-json", action="store_true",
                    help="THE MATRIX GATE (Phase 2b task 7, spec section "
                         "5): the full 910-cell --to-json cross-product "
                         "(13 fixtures x 35 target/subtarget pairs x "
                         "v2/v3) against fresh reference drf.exe runs, "
                         "preceded by the BLANK_EN/NEXTDAAD pair gate's "
                         "own composition check and followed by the "
                         "flag extras (TO_JSON_EXTRA_JOBS); needs the "
                         "reference toolchain (oracle.local.json) and "
                         "--ndrc")
    ap.add_argument("--to-json-compare", nargs=2, metavar=("A", "B"),
                    help="CI leg of the pair gate (spec section 6): "
                         "compare two JSON files after clock-symbol "
                         "normalisation only, no reference toolchain "
                         "needed - see to_json_compare_files")
    ap.add_argument("--to-json-goldens", action="store_true",
                    help="CI leg of the matrix gate: run ndrc --to-json "
                         "against every committed fixture JSON and byte-"
                         "compare, clock-normalised on both sides; no "
                         "reference toolchain needed, requires --ndrc")
    ap.add_argument("--join", action="store_true",
                    help="the join gates: the GOLDEN leg (Phase 3 task 4, "
                         "Gate 10) - every curated_jobs() pair run through "
                         "the join CLI (ndrc TARGET [SUB] LANG in.DSF "
                         "[out.ddb] [options]) and byte-compared to the "
                         "committed goldens, no reference toolchain needed "
                         "- then the LIVE DDB matrix leg (task 5), the "
                         "910-cell grid against the reference drf+drb "
                         "flow, then the flag extras (JOIN_EXTRA_JOBS), "
                         "both of which do need the reference toolchain "
                         "(oracle.local.json) and PHP; requires --ndrc")
    ap.add_argument("--goldens-only", action="store_true",
                    help="with --join, run the goldens leg alone and skip "
                         "the live matrix leg - the switch shape CI's Gate "
                         "10 uses, CI having no reference toolchain")
    ap.add_argument("--only", action="append", default=[],
                    help="restrict --from-json to pairs whose 'fixture/"
                         "slug' contains this substring, --to-json to "
                         "cells whose '<FIXTURE>_<TARGET>[_<SUB>]_<v2|v3>' "
                         "id contains it, or --join to golden-leg pairs "
                         "whose 'fixture/slug' contains it AND live-leg "
                         "cells whose cell id contains it AND flag extras "
                         "whose label contains it; repeatable, AND "
                         "semantics (every substring given must match)")
    args = ap.parse_args(argv)

    if args.to_json_compare:
        a, b = args.to_json_compare
        return to_json_compare_files(Path(a), Path(b))

    if not (args.self_check or args.ndrc is not None or args.from_json
            or args.to_json or args.to_json_goldens or args.join):
        ap.error("one of --self-check, --ndrc, --from-json, --to-json, "
                 "--to-json-goldens, --join or --to-json-compare is "
                 "required")
    if args.from_json and args.ndrc is None:
        ap.error("--from-json requires --ndrc")
    if args.to_json and args.ndrc is None:
        ap.error("--to-json requires --ndrc")
    if args.to_json_goldens and args.ndrc is None:
        ap.error("--to-json-goldens requires --ndrc")
    if args.join and args.ndrc is None:
        ap.error("--join requires --ndrc")
    if args.goldens_only and not args.join:
        ap.error("--goldens-only requires --join")
    if args.only and not (args.from_json or args.to_json or args.join):
        ap.error("--only requires --from-json, --to-json or --join")

    # Every gate below the plain --ndrc one spawns ndrc with cwd set to a
    # fresh run directory, so a RELATIVE --ndrc path (CI passes "./ndrc")
    # would be resolved against that temp directory rather than against
    # the caller's own cwd and vanish - CreateProcess and POSIX
    # fork+chdir+exec both, for their own different reasons. Resolve it
    # once here, against the cwd the user actually typed it in, exactly
    # as test_cli.py's own _resolve_ndrc does for the same reason.
    if args.ndrc is not None:
        args.ndrc = args.ndrc.resolve()

    if args.to_json_goldens:
        return to_json_goldens_check(args.ndrc)
    if args.join:
        # The goldens leg first - it needs no reference toolchain, so a
        # machine without one still gets it - then the live DDB matrix
        # leg, unless --goldens-only (CI's own invocation shape) says to
        # stop there.
        rc_goldens = join_goldens_check(args.ndrc, args.only)
        if args.goldens_only:
            return rc_goldens
        rc_live = join_matrix_check(args.ndrc, args.only)
        # The flag extras run AFTER the 910 cells, with their own summary
        # line, each gated by --only against its own label the way the
        # --to-json extras are.
        rc_extras = join_extras_check(args.ndrc, args.only)
        if 2 in (rc_live, rc_extras):
            return 2
        return 1 if (rc_goldens or rc_live or rc_extras) else 0
    if args.to_json:
        # The 2a pair gate still runs first, for the one leg the matrix
        # has no counterpart for: the COMPOSITION check (ndrc's own JSON
        # fed back through ndrc --from-json must reproduce the committed
        # BLANK_EN NEXTDAAD golden). It is gated by --only against its
        # own cell id so a sliced sweep does not drag it in.
        rc_pair = 0
        if _cell_selected("BLANK_EN_NEXTDAAD_v3", args.only):
            rc_pair = to_json_check(args.ndrc)
        rc_matrix = to_json_matrix_check(args.ndrc, args.only)
        # The flag extras run AFTER the 910 cells, with their own summary
        # line, each gated by --only against its own label the way
        # flags_extra_check's pairs are.
        rc_extras = to_json_extras_check(args.ndrc, args.only)
        if 2 in (rc_pair, rc_matrix, rc_extras):
            return 2
        return 1 if (rc_pair or rc_matrix or rc_extras) else 0
    if args.from_json:
        # task-7-brief.md Step 5: the two fixed flag-extra pairs are added
        # to the sweep as fixed extras, each gated by --only the same way
        # curated_jobs() pairs are (see flags_extra_check's docstring for
        # the label-based selection this uses instead of "fixture/slug").
        rc_curated = from_json_check(args.ndrc, args.only)
        rc_flags = flags_extra_check(args.ndrc, args.only)
        if rc_curated == 2 or rc_flags == 2:
            return 2
        return 1 if (rc_curated or rc_flags) else 0
    if args.self_check or args.ndrc is None:
        return self_check()
    return verify_ndrc(args.ndrc)


if __name__ == "__main__":
    raise SystemExit(main())
