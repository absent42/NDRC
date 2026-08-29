# NDRC - Next DAAD Reborn Compiler

NDRC compiles DAAD Adventure Writer source (.DSF) into a DAAD database
(.DDB). It is one self-contained executable with no runtime, no
interpreter and no companion installation, for Windows and Linux.

It is a from-scratch C port of DRC, Uto's DAAD Reborn Compiler (a
FreePascal front end and a PHP back end), verified byte-identical
against DRC's own drf+drb pipeline. Uto's DRC is the reference
implementation: https://github.com/Utodev/DRC

NDRC was made to streamline the toolchain of [NextDAAD](https://github.com/absent42/NextDAAD), however the compiler 
is compliant with all DAAD targets and can be used as a substitute for DRC.

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

## Text compression: -auto-tokens

    ndrc NEXTDAAD EN game.DSF -auto-tokens

NDRC v0.2+ offers enhanced text compression compared to DRC, enabling DAAD DDBs to fit more game text into the hard limit database sizes. This is compatible with existing DAAD interpreters, just requiring an extra CLI flag when compiling your game with NDRC.

DRC compresses text with a fixed token table per language - the same
128 English digraphs for every English game ever compiled. With
`-auto-tokens`, NDRC instead selects up to 128 tokens from the
compiling game's own text and encodes the text with an optimal parse
over that table. Compressed text measures 8-14% smaller than the
builtin table across a corpus of real games, with the biggest wins on
prose-heavy games near the 64K database ceiling. Selection is
data-driven, so it adapts to any of the five languages - and to your
game's actual vocabulary - automatically.

The output DDB is format-identical: it decodes on every DAAD
interpreter, on every target, including the original 8-bit
interpreters. Without the flag, NDRC's output remains byte-identical
to DRC, as always.

Add `--tok` to also write the selected table as a standard `.tok`
file beside the source. The normal override lookup picks it up on the
next run and reproduces the same DDB byte for byte, and the same file
works in stock DRC.

Compared with [DRT](https://github.com/daad-adventure-writer/DRT),
the existing per-game tokenizer for DRC: on the same corpus of real
games, DRT's tables came out roughly even with DRC's builtin English
table, because its greedy selection splits the text on each chosen
token and locks out the short high-frequency fragments that carry a
good table. NDRC scores every candidate against an optimal parse of
the whole text instead, which is where the 8-14% comes from - and it
runs inside the compile, no separate tool or Python needed.
DRT-generated `.tok` files remain fully supported as overrides,
unchanged.

See [docs\cli.md](docs/cli.md), "Per-game token selection", for the
freeze workflow and the classic-target details.

## Building from source

Requires a C17 compiler, no other dependencies. On Windows, run
make.bat, which locates MinGW-w64 for you:

    make.bat test

Elsewhere, with a C17 compiler and GNU make:

    make test

## Other DAAD projects

- [NextDAAD](https://github.com/absent42/NextDAAD) - ZX Spectrum Next DAAD interpreter
- [DAAD DSF](https://github.com/absent42/DAAD-DSF) - VS Code extension with DAAD map preview, compiler-parity diagnostics and syntax highlighting

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

NDRC is a from-scratch C port of DRC, Copyright (C) Uto, also
GPL-3.0.
