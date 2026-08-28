# Changelog

## v0.1.1 - unreleased

- The verbose (`-v`) endianness line now names the real file byte
  order. It previously printed the inverse ("big endian" for
  little-endian targets and vice versa), reproducing a display bug in
  the DRC reference that Uto fixed upstream in commit ff45ff2.
  Cosmetic only: emitted DDB bytes are unchanged for every target.

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
