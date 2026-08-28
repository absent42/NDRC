# NDRC - Next DAAD Reborn Compiler

NDRC compiles DAAD Adventure Writer source (.DSF) into a DAAD database
(.DDB). It is one self-contained executable with no runtime, no
interpreter and no companion installation, for Windows, Linux and
macOS.

It is a from-scratch C port of DRC, Uto's DAAD Reborn Compiler (a
FreePascal front end and a PHP back end), verified byte-identical
against DRC's own drf+drb pipeline. Uto's DRC is the reference
implementation: https://github.com/Utodev/DRC

## Quick start

The latest release can be downloaded from the [releases page](https://github.com/absent42/NDRC/releases).

    ndrc NEXTDAAD EN game.DSF

This compiles game.DSF for the NEXTDAAD target, English language, and
produces game.DDB in the same directory.

## Targets

ZX, NEXTDAAD, CPC, C64, CP4, CPM, MSX, MSX2, ZX81, PCW, PC, AMIGA, ST,
HTML.

See [docs\cli.md](docs/cli.md) for the full target/subtarget list, invocation
grammar and option reference.

## Building from source

Requires a C17 compiler, no other dependencies. On Windows, run
make.bat, which locates MinGW-w64 for you:

    make.bat test

Elsewhere, with a C17 compiler and GNU make:

    make test

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

NDRC is a from-scratch C port of DRC, Copyright (C) Uto, also
GPL-3.0.
