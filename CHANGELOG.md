# Changelog

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
