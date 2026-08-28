# NDRC differential oracle

Compares NDRC output against reference DRC, which remains the definition
of correct output until NDRC is proven equivalent.

## Setup

Copy `oracle.local.json.example` to `oracle.local.json` and set the paths
for this machine. That file is gitignored.

The DRF binary must carry the NEXTDAAD target. The DRF shipped with DAAD
Ready does not have it and silently compiles NEXTDAAD at COLS=42 instead
of 80. Build one from the DRC fork:

    cd <drc-fork>/src && fpc drf.pas

## Layout

    ndrcoracle/config.py     locates the reference toolchain
    ndrcoracle/matrix.py     the 35 target/subtarget pairs and the axes
    ndrcoracle/reference.py  drives drf.exe then php drb.php
    ndrcoracle/diffreport.py byte diffs with offsets
    ndrcoracle/divergence.py the expected-divergence registry
    test_matrix.py           self-test for the matrix enumeration
    test_diffreport.py       self-test for byte-diff reporting
    test_divergence.py       self-test for the expected-divergence registry
    verify.py                verifies ndrc against the committed goldens;
                              --self-check checks goldens against the manifest;
                              --from-json --ndrc <path> is the maintainer gate
                              for ndrc --from-json (needs the reference
                              toolchain)
    gen_goldens.py           regenerates the committed goldens from the
                              reference toolchain; --check re-checks them

## The --from-json maintainer gate

    python verify.py --from-json --ndrc ../../ndrc.exe

Needs the reference toolchain (`oracle.local.json`), unlike every other
mode here, so it is not part of CI. It rebuilds the one fixture/combo
pair NDRC's back end covers in Phase 1a - BLANK_EN, NEXTDAAD/EN/v3/opt -
from a FRESH reference DRF run (not the committed fixture JSON), then
runs reference DRB `-v` and `ndrc --from-json -v` on that same fresh
JSON and checks three things: ndrc's DDB equals the committed golden,
ndrc's DDB equals the fresh reference DDB, and the two `-v` transcripts'
condact/section maps (map lines, `Warning: token` lines and the
`Compression tokens used:` line) are identical in order.

Any DDB mismatch is reported through `diffreport.format_diff` with
section attribution: each differing run is annotated with the DDB
section it falls in (`header`, `tokens`, `vocabulary`, and so on),
derived from the 13 header words the reference patches at output offset
8 (analysis S4.1) in the EXPECTED file, sorted by address into ranges.

## Reference quirks handled here

- `drb.php:1240` prints a stray debug line for subtargeted runs. Captured,
  never parsed.
- `drf.pas:259-262` injects clock-derived symbols. Any future JSON
  comparison must neutralise them before comparing; that canonicalisation
  is deferred to Phase 2 and is not implemented yet.
- Both stages write fixed-name files (`g.json`, `g.DDB`, DRF's `g.___`)
  into the current directory, so every run gets its own directory.
- `drb.php` writes `0.XMB` only under `-X`, which the oracle never
  passes, so no XMB file is produced or collected.
