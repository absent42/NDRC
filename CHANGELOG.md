# Changelog

## v0.4.0 - 29/10/2026

- `-auto-tokens` compresses object texts too: they join the
  compressable set under the optimal encoder.
- `-auto-tokens` emits identical texts within a table once: the
  lookup table points every duplicate at the one copy.
- A marked `.tok` (`"encoder": "optimal"`) compiles with both on
  reload, so `-auto-tokens` and marked-.tok output is smaller than the
  0.3.0 build produced, and decodes identically. Unmarked and
  hand-written `.tok` files and the builtin table compile exactly as
  DRC would.
- New `-no-object-tokens` option keeps object texts uncompressed under
  `-auto-tokens` or a marked `.tok`, for interpreters the object-text
  compression was not verified on; sharing stays on. A `--tok` tee
  records it as `"objects": false`.
- On targets other than NEXTDAAD, object texts keep their first word,
  the spaces after it and every `.` uncompressed (a text with no space
  stays uncompressed), because the original interpreters strip the
  article by scanning stored bytes for the first space.
- Under `-auto-tokens` and a marked `.tok` the token table is emitted
  most-referenced first, so interpreters scanning the table find common
  tokens sooner. Format unchanged.
- The compiler verifies the emitted DDB by decoding every text through
  its lookup table before writing it.
- The DDB stays format-identical and decodes on every DAAD
  interpreter. Verified on NextDAAD hardware and, under emulation, on
  the original ZX Spectrum 48K, Amstrad CPC, MSX1 and PCDAAD
  interpreters with a fixture exercising compressed object texts,
  `_` substitution, articles, More... inside lists and every shared
  duplicate.
- New `-nxtext` option (NEXTDAAD only, implies `-auto-tokens`) emits
  NextDAAD text format 2 - Huffman-coded texts and XMB with an indexed
  dictionary of up to 383 tokens, version byte bit 7 set - whenever
  the selection finds a token; `-v` reports a build that came out
  larger than format 1. `-x` and `--tok` are refused with it, and
  `-no-object-tokens` has no effect when format 2 is emitted. Only
  NextDAAD 0.13.0 or later reads format 2; every other interpreter, and
  older NextDAAD releases, refuse the file.
- Under `-nxtext` the coded texts live in a text region after part A
  (header, tables, vocabulary, processes). Part A and the region are
  each limited to 65535 bytes, so a DDB can reach 131070. The size
  report adds `Part A is <n> bytes, text region is <n> bytes.`, `-v`
  adds a `Text region` line to the address map, and a region that does
  not fit is `Text region goes <n> bytes over the 65535 limit`.
- Under `-nxtext` the XMB is Huffman-coded too and starts with an "X2"
  header carrying the text block's fingerprint, so a stale XMB is
  detected. An XMESSAGE whose coded text exceeds 8K is an error.
- The self-check decodes the format 2 text block, every coded text and
  every XMB stream before the DDB is written.
- `-v` reports each table's shared duplicates (`Shared <n> duplicate
  texts in <table> (<n> bytes).`) and the token selection
  (`Auto-tokens: <n> candidates, <n> selected.`, `Token table ordered
  by use.`).
- Text format 2 and the text region were verified on NextDAAD hardware
  with fixtures covering object, location and message texts,
  XMESSAGEs, a DDB over 64K, a part switch into it and each refusal
  case.

## v0.3.0 - 25/10/2026

- New `-srcmap=<path>` option: writes a DSM v2 source map beside the
  DDB, pairing each emitted condact, entry and process with its DSF
  file, line and column, plus the `#define` symbols. It is read by the
  NextDAAD CSpect debugger for source-level breakpoints and stepping.
  Case-sensitive, NDRC extension. The DDB is byte-identical with or
  without it. Needs a .DSF source input: `--from-json` refuses it.
  With `-np` no map is written, because that option stops the compile
  early.

## v0.2.2 - 26/09/2026

- The linux64 release binary is linked statically. A dynamic link on the
  build runner bound fmod@GLIBC_2.38 and failed on glibc 2.35 (Ubuntu
  22.04); the static build runs on any x86_64 glibc distro and produces
  byte-identical DDBs. Windows and source builds are unchanged.
- Entries that differ only in indirection no longer share one compiled
  body. Entry sharing ignored `@` on either parameter, so `MES @x` could
  run as `MES x` and `LET a @b` could lose its `INDIR` prefix. This
  reproduced a bug in the DRC reference that Uto fixed upstream in
  commit 71df4e5; output stays byte-identical to DRC.

## v0.2.1 - 30/08/2026

- New `-cols=40|80` option: overrides the exported `COLS` symbol for
  NEXTDAAD 40-column text mode. Case-sensitive, NDRC extension.
  Pair with the game issuing `GFX 1 18` at init. Any other value is
  an "Invalid option" error. Without the flag, `COLS` is unchanged
  (NEXTDAAD's builtin 80).

## v0.2 - 29/08/2026

- New `-auto-tokens` option: per-game text compression. Instead of
  DRC's fixed per-language token table, the compiler selects up to 128
  tokens from the compiling game's own text and encodes the text with
  an optimal parse. Compressed text measures 8-14% smaller than the
  builtin table across a corpus of real games. Output DDBs remain
  format-identical and decode on every DAAD interpreter, on every
  target; without the flag, output stays byte-identical to DRC.
- New `--tok[=path]` option (implies `-auto-tokens`): writes the
  selected table as a standard `.tok` file beside the input, where the
  normal override lookup finds it on the next run. The file records
  `"encoder": "optimal"` so a flagless recompile reproduces the same
  DDB byte for byte; stock DRC accepts the same file and simply
  ignores the marker.
- A `.tok` override carrying `"encoder": "optimal"` (with advanced
  compression) engages the optimal-parse encoder even without the
  flag. Hand-written and DRT `.tok` files without the marker compile
  exactly as DRC would.
- On targets other than NEXTDAAD, selected tokens never contain `_`
  or `@`: verified against the original ZX Spectrum interpreter, a
  `_` arriving from inside a token prints literally instead of
  substituting the object name.
- Every `-auto-tokens` compile self-checks: each compressed message is
  decoded back and compared against the source text before the DDB is
  written.
- The verbose (`-v`) endianness line now names the real file byte
  order. It previously printed the inverse ("big endian" for
  little-endian targets and vice versa), reproducing a display bug in
  the DRC reference that Uto fixed upstream in commit ff45ff2.
  Cosmetic only: emitted DDB bytes are unchanged for every target.
  (Was staged as v0.1.1, never released.)

## v0.1 - initial release

- Single-binary DAAD compiler: `ndrc <TARGET> [subtarget] <LANG>
  <in.DSF> [out.ddb]` compiles DSF source to a DDB database in one
  pass, byte-identical to the DRC reference pipeline (drf + drb).
- `--to-json` and `--from-json` split modes, matching the reference
  front and back ends individually; `--json[=path]` writes the
  intermediate JSON alongside a joined compile.
- All 14 DRC targets and their subtargets, the five token-table
  languages, and the full option set of both reference stages.
- See docs/cli.md for the complete reference.
