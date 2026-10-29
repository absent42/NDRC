# NDRC - Next DAAD Reborn Compiler

NDRC compiles DAAD Adventure Writer game source code (.DSF) into a DAAD database (.DDB). NDRC is one self-contained executable with no other dependencies or installation requirements, for Windows and Linux. It is also features increased text compression for all DAAD targets compared to DRC, and with further enhanced text compression and larger database sizes for NextDAAD.

It is a from-scratch C port of DRC, Uto's DAAD Reborn Compiler (a
FreePascal front end and a PHP back end), verified byte-identical
against DRC's own drf+drb pipeline when using standard compile mode. Uto's DRC is the reference
implementation: https://github.com/Utodev/DRC

NDRC was made to streamline the toolchain of [NextDAAD](https://github.com/absent42/NextDAAD) and provide a NextDAAD specific target supporting larger databases than other ZX targets. It is included with the NextDAAD authoring kit, so there's no need to download separately.

The compiler is also compatible with all standard DAAD targets and can be used as a substitute for DRC whilst giving improved text compression through the use of the auto-tokens option.

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

NDRC v0.2+ offers enhanced text compression compared to DRC, enabling
DAAD DDBs to fit more game text into the hard limit database sizes.
This is compatible with existing DAAD interpreters, just requiring an
extra CLI flag when compiling your game with NDRC.

DRC compresses text with a fixed token table per language - the same
128 English digraphs for every English game ever compiled. With
`-auto-tokens`, NDRC instead selects up to 128 tokens from the
compiling game's own text and encodes the text with an optimal parse
over that table. Selection is data-driven, so it adapts to any of the
five languages - and to your game's actual vocabulary - automatically.

NDRC v0.4 extends `-auto-tokens` further, still format-identical:

- **Object texts are compressed too.** They join the same per-game
  table as locations, messages and system messages. On targets other
  than NEXTDAAD, each object text keeps its first word, the spaces
  after it and every `.` uncompressed, because the original
  interpreters find the article by scanning the stored bytes for the
  first space. Add `-no-object-tokens` to keep object texts
  uncompressed for an interpreter this was not verified on.
- **Duplicate texts are stored once.** Identical texts within a table
  share one copy, and the lookup table points every duplicate at it.
- **The token table is ordered by use.** The most-referenced tokens
  come first, so interpreters that scan the table find common tokens
  sooner and print faster.

The output DDB decodes on every DAAD interpreter, on every target,
including the original 8-bit interpreters. It has been verified on
NextDAAD hardware and, under emulation, on the original ZX Spectrum
48K, Amstrad CPC, MSX1 and PCDAAD interpreters. The compiler also
checks its own output by decoding every text back before writing the
DDB. Without the flag, NDRC's output remains byte-identical to DRC, as
always.

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
the whole text instead, and it runs inside the compile, no separate
tool or Python needed. DRT-generated `.tok` files remain fully
supported as overrides, unchanged.

See [docs\cli.md](docs/cli.md), "Per-game token selection", for the
freeze workflow and the classic-target details.

## NextDAAD text format 2: -nxtext

    ndrc NEXTDAAD EN game.DSF -nxtext

NDRC v0.4+ can emit a NextDAAD-only text format that goes well beyond
what the classic DDB format allows. It needs a NextDAAD release that
reads text format 2; every other interpreter, and older NextDAAD
releases, refuse the file cleanly.

- **Huffman-coded text.** Every text, including XMESSAGEs in the XMB
  file, is stored as a canonical Huffman bit stream, so common letters
  and tokens cost fewer than 8 bits.
- **A bigger, faster dictionary.** Up to 383 per-game tokens instead of
  128, and a token may reuse another token inside it. Each token is
  found through an index rather than by scanning the table, so text
  prints faster: in CSpect a full location description in Rabenstein
  prints in 27 ms against 68 ms for the same game in the classic
  format.
- **Texts past 64K.** The coded texts move out of the 64K database
  into a text region of their own, so a game gets up to 64K for its
  logic (header, vocabulary, processes) and up to another 64K for its
  text: a DDB can reach 131,070 bytes. The game's logic keeps the room
  the texts used to take.

`-nxtext` implies `-auto-tokens` and works for the NEXTDAAD target
only. Small games get format 2 too, for the print speed; under `-v`
the compiler reports when format 2 came out larger than the classic
format. See [docs\cli.md](docs/cli.md), "Text format 2", for the
details and the refused option combinations.

## Compression comparison

Game data size in bytes, compiled by NDRC v0.4.0: the whole DDB, plus
`0.XMB` for the games marked † that keep text in XMESSAGEs. "DRC"
is NDRC with no compression option, which is byte-identical to DRC.
The classic column uses the PC target, because the largest games do
not fit the ZX 48K memory map; every classic target saves a similar
share.

| Game | Classic DRC | Classic `-auto-tokens` | NextDAAD DRC | NextDAAD `-auto-tokens` | NextDAAD `-nxtext` |
|------|------:|------:|------:|------:|------:|
| Rollercoaster | 10,346 | 9,680 (-6.4%) | 10,029 | 9,372 (-6.6%) | 9,296 (-7.3%) |
| Abracadabra 1 (Spanish) | 15,722 | 14,790 (-5.9%) | 15,235 | 14,249 (-6.5%) | 13,906 (-8.7%) |
| Dracula Part 1 | 19,334 | 18,034 (-6.7%) | 19,087 | 17,793 (-6.8%) | 16,709 (-12.5%) |
| The Greedy Dwarf | 22,026 | 19,988 (-9.3%) | 21,417 | 19,337 (-9.7%) | 18,548 (-13.4%) |
| Rabenstein | 23,198 | 21,582 (-7.0%) | 22,756 | 21,072 (-7.4%) | 19,495 (-14.3%) |
| Voyage of the Beano † | 31,542 | 29,458 (-6.6%) | 30,767 | 28,668 (-6.8%) | 25,942 (-15.7%) |
| The Beast \* | 38,632 | 35,380 (-8.4%) | 38,213 | 34,921 (-8.6%) | 29,650 (-22.4%) |
| The Secret of Barron Wood † | 44,187 | 43,619 (-1.3%) | 43,742 | 43,152 (-1.3%) | 26,238 (-40.0%) |
| Murder Mystery Weekend \* | 64,182 | 59,296 (-7.6%) | 63,486 | 58,449 (-7.9%) | 49,065 (-22.7%) |
| Rambo † ‡ | - | - | 116,109 | 115,685 (-0.4%) | 86,774 (-25.3%) |

\* The full text of the original PAW game in a minimal DSF shell, with
no game logic. These two show the near-ceiling case: Murder Mystery
Weekend compiles to within 2K of the 64K limit with DRC, and `-nxtext`
frees 14K of that.

† These games keep text in XMESSAGEs, in `0.XMB`. `-auto-tokens`
compresses only the DDB's texts, while `-nxtext` Huffman-codes the XMB
too, which is why Barron Wood and Rambo, with most of their text there,
gain so much more from it: Barron Wood's XMB shrinks from 29,003 to
11,140 bytes.

‡ Rambo is an unreleased Pascal to DAAD port of the Mindscape text 
adventure, DDB alone is 65,283 bytes with DRC for NextDAAD, 252
bytes under the 64K limit, and too big for the PC target. With
`-nxtext` its logic takes 61,942 bytes and its texts move to a
4,538-byte text region, leaving the logic 3.5K of room to grow.

The percentages are of the whole game data, which also holds vocabulary,
processes and other data that compression does not touch, so the
saving on the text itself is larger. Format 2 gains most on large,
prose-heavy games, where its bigger dictionary and Huffman coding have
the most text to work on.

## Source maps: -srcmap

    ndrc NEXTDAAD EN game.DSF -srcmap=game.dsm

NDRC v0.3+ can write a DSM v2 source map alongside the DDB. It records
the DSF file, line and column of every emitted condact, entry and
process, plus the `#define` symbols, so a debugger can map DDB offsets
back to source. The DDB itself is byte-identical with or without the
flag. See [docs\cli.md](docs/cli.md) for the details and limits.

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
