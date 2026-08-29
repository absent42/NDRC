# NDRC CLI reference

NDRC (Next DAAD Reborn Compiler) is a from-scratch C port of DRC, Uto's DAAD Reborn Compiler,
verified byte-identical against DRC's own drf+drb pipeline (the
reference implementation: https://github.com/Utodev/DRC).
DRC ships as two programs (a drf front end and a drb back end) joined
by a JSON file on disk; ndrc runs both stages in one process by
default, and also exposes each stage on its own for tooling that wants
the JSON intermediate directly.

Every command prints one banner line (`NDRC 0.1` or `NDRC 0.1
--to-json`/`--from-json`) before anything else; that line is omitted
from the examples below.

## Join invocation (the normal case)

    ndrc TARGET [SUBTARGET] LANG file.dsf [output.ddb] [symbols] [options]

Compiles a .DSF straight to a .DDB in one process: the drf stage reads
and checks the source, the drb stage emits the database. Example:

    ndrc NEXTDAAD EN game.DSF

writes `game.DDB` beside `game.DSF`. With no output name, the DDB name
is the input's own name with its extension replaced by `DDB`
(upper-case).

### Targets and subtargets

14 targets. Four take a mandatory subtarget (given immediately after
the target); the rest take none.

| Target | Subtarget required | Subtarget values |
|---|---|---|
| NEXTDAAD | no | - |
| ZX | yes | 48K, 128K, PLUS3, ESXDOS, NEXT, UNO |
| ZX81 | yes | 16K, SD81B |
| PC | yes | VGA256, VGA, EGA, CGA, TEXT |
| MSX2 | yes | 5_6, 5_8, 6_6, 6_8, 7_6, 7_8, 8_6, 8_8, 10_6, 10_8, 12_6, 12_8 |
| CPC | no | - |
| C64 | no | - |
| CP4 | no | - |
| CPM | no | - |
| MSX | no | - |
| PCW | no | - |
| AMIGA | no | - |
| ST | no | - |
| HTML | no | - |

Target and subtarget names are case-insensitive. The target name
itself is not validated by the source-reading stage - an unrecognised
target compiles the source anyway and is only rejected once the
database-writing stage sees it.

For any of the four subtargeted targets, the database-writing stage
always prints a stray `Debug: Checking subtarget <sub> for target
<target>` line before its own `Target:` line, win or lose - this is
ported output, not something to act on.

### Language

One of: EN, ES, DE, PT, FR. Case-insensitive.

### Grammar after the input file

The argument immediately after the input file is claimed as the output
name if it contains a dot anywhere; otherwise it is left for the
options loop below, where a dotless argument becomes the
additional-symbols list (see below). A `--json=<path>` argument is the
one exception - it always contains a dot but is never claimed as the
output name.

    ndrc NEXTDAAD EN game.DSF out.ddb          out.ddb is the output
    ndrc NEXTDAAD EN game.DSF EXTRA1,EXTRA2    both become #define symbols
    ndrc NEXTDAAD EN game.DSF out.ddb EXTRA1   both output name and symbols

Only the last dotless argument survives as the symbol list; symbols are
comma-separated and each becomes a defined constant, numbered by its
position in the list (1, 2, 3...).

### Options (source-reading stage)

Case-sensitive, dash-prefixed, recognised in any position after the
input file.

| Option | Meaning |
|---|---|
| `-verbose` | Print extra progress detail (target/subtarget line, stage confirmations). |
| `-no-semantic` | Skip semantic analysis (only syntax is checked). Cannot combine with `-semantic-warnings`. |
| `-semantic-warnings` | Report semantic problems as warnings rather than errors, so compilation still completes. Cannot combine with `-no-semantic`. |
| `-force-normal-messages` | Force normal (non-extended) message handling. Cannot combine with `-force-x-messages`. |
| `-force-x-messages` | Force extended-message (XMessage) handling. Cannot combine with `-force-normal-messages`. |
| `-check-maluva-disabled` | Disable the MALUVA extension check. |
| `-v3` | Generate a DAAD V3 database. Default is V2. |
| `-7` | Generate a 7-bit ASCII database. |
| `-replace-xcondacts` | Replace extended condacts with their standard equivalents. |

Options are matched left to right in one pass; an option's own
confirmation line (when `-verbose` is on) reflects verbose's setting
at that point in the argument list, not its final value - so option
order can change what gets printed, though never what gets compiled.

### Options (database-writing stage)

Case-insensitive (upper-cased before matching), dash-prefixed.

| Option | Meaning |
|---|---|
| `-v` | Verbose output: per-section memory-map addresses, classic/debug mode and endianness status lines, and an "Adventure Totals" + created-file summary at the end. The size-per-block breakdown and final database size/address lines print regardless of `-v`. |
| `-c` | Force classic mode (pre-DRC compatible output). Refused on NEXTDAAD - see below. |
| `-ch` | Prepend a C64 header. Valid only for the C64 and CP4 targets. |
| `-3h` | Prepend a +3DOS header. Valid only for the ZX target. |
| `-d` | Force debug mode (extra debug information for the ZEsarUX debugger). On a target that does not support it, prints `Debug mode active, but target is not ZX. Debug mode deactivated.` and turns debug mode back off - the message says "not ZX" even on a target other than ZX that also lacks debug support (e.g. CP4); this is the printed text verbatim, not a target-name bug in ndrc. |
| `-np` | Force no padding. Cannot combine with `-p`. WARNING: this flag writes whatever bytes have been emitted so far and stops immediately at the very first padding decision - the database is truncated mid-emission, well short of complete, and the run still reports success (exit 0) with no error. Reproduced from the reference implementation. Do not use. |
| `-p` | Force padding. Cannot combine with `-np`. |
| `-x` | Dump TX (text) sections to a separate `0.XMB` file instead of embedding them in the database. |
| `-b=<addr>` | Override the target's default base address. Decimal, or hex with a `0x`/`0X` prefix. Must resolve to 1-0xFFFF. |
| `-auto-tokens` | Select compression tokens from the game's own text instead of the builtin language table. Case-sensitive, NDRC extension. Accepted on the join and `--from-json`. Wins over an implicit `.tok` beside the input (a notice names the bypassed file). Encodes text with an optimal parse (smaller output than the sequential encoder); output DDBs remain format-identical and decode on every DAAD interpreter. |
| `--tok[=path]` | Write the selected token table as a standard `.tok` file (bare form: `<input>.tok`, where the normal override lookup finds it on the next run). Implies `-auto-tokens`. Like `--json=`, a `--tok=` argument is never claimed as the output name. |

An unrecognised option, in either set, stops compilation immediately
with an error naming it. A stray extra positional argument is not an
error here: the join and `--to-json` absorb every dotless positional
into the additional-symbols slot silently (only the last one is kept -
see above), and a second dotted/dashed positional is likewise just
whatever the grammar above already assigns it to. The database-writing
stage's own standalone CLI (`--from-json`) is the one place a second
stray positional is rejected outright - see below.

## Per-game token selection

`-auto-tokens` replaces the builtin language token table with up to 128
tokens chosen from the compiling game's own text, then encodes the text
with an optimal parse over that table. On real English games this
measured 7.8% to 14.4% off compressed text+table against the builtin
table. Selection is deterministic: the same source always produces the
same table.

Without the flag, output is byte-identical to DRC, as always. A
compile with the flag self-checks: every compressed message is decoded
back and compared against the source text before the DDB is written.

The freeze workflow: compile once with `-auto-tokens --tok`, then drop
both flags - the written `.tok` sits beside the input where the normal
override lookup picks it up. The tee records `"encoder": "optimal"`
and the reload honours it, so the frozen build is byte-identical to the
flagged one. Hand-written and DRT `.tok` files without the marker
compile exactly as DRC would; stock DRC accepts a marked `.tok` and
simply ignores the field.

On targets other than NEXTDAAD, selected tokens never contain `_` or
`@` (object-name and print placeholders): on the original ZX Spectrum
interpreter a `_` arriving from inside a token prints literally
instead of substituting the object name, so such tokens would silently
change the game's output. NEXTDAAD expands tokens byte-transparently
and has no such restriction.

### `--json[=path]`

Available only on the join invocation. Tees the JSON the source-reading
stage produces to a file, without changing the compile in any other
way:

    ndrc NEXTDAAD EN game.DSF --json              writes game.json
    ndrc NEXTDAAD EN game.DSF --json=dump.json     writes dump.json

Bare `--json` writes `<input>.json` (the same default name the flow
would use internally) beside the input file. The tee is written as soon
as the source-reading stage finishes, so it is still produced even if
the database-writing stage later refuses the input.

## `--to-json` (source-reading stage only)

    ndrc --to-json TARGET [SUBTARGET] file.dsf [output.json] [symbols] [options]

Runs only the drf-equivalent stage: reads and checks a .DSF and writes
DRC's JSON intermediate format, without producing a database. This is
DRC's own front-end/back-end interchange format - other tools in the
DAAD ecosystem may read or write it directly. Grammar, target list,
language rules (there is no language argument in this mode - language
is a database-writing-stage concern) and the source-reading options
table above all apply unchanged. With no output name, the JSON is
written as `<input>.json`.

## `--from-json` (database-writing stage only)

    ndrc --from-json TARGET [SUBTARGET] LANG input.json [output.ddb] [options]

Runs only the drb-equivalent stage: reads a JSON document in DRC's
intermediate format and writes a .DDB. The target/subtarget/language
rules and the database-writing options table above apply unchanged.
With no output name, the DDB is written as `<input>.DDB` (extension
replaced, upper-case).

Unlike the join and `--to-json`, a missing required argument here
(target, subtarget, language or input file) is reported as
`Error: usage: ndrc --from-json ...` and exits 2, not 1 - see Exit
codes below.

The first non-option argument after the input file is the output
name; a second one is rejected outright with
`Error: Bad parameter: <argument>.`, exit 2 - unlike the join and
`--to-json`, where an extra dotless positional is just silently
absorbed as the (last-wins) symbol list.

## The NEXTDAAD / `#classic` refusal

A source file's `#classic` directive (or the `-c` option) asks for
classic, pre-DRC-compatible output. This is refused for the NEXTDAAD
target specifically, because the original pre-DRC interpreters cannot
read a NEXTDAAD database at all - the machine byte and pointer base
both differ. The database-writing stage reports:

    Error: #classic is not supported on NEXTDAAD: the original pre-DRC
    interpreters cannot read a NEXTDAAD database at all, since the
    machine byte and pointer base both differ.

and exits with code 2. `-c` cannot override this refusal.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. |
| 1 | The join's or `--to-json`'s own too-few-arguments usage message, or a compile-time syntax/semantic error in the source. |
| 2 | A parameter error (bad option, bad file, bad grammar), `--from-json`'s own too-few-arguments usage message, or a database-writing-stage error (invalid target/subtarget/language, refused input, I/O failure). |

The usage message is not a fixed exit class: the join and `--to-json`
print their own text directly and exit 1 when too few positional
arguments are given; `--from-json` instead reaches its usage text
through the database-writing stage's own error path (`Error: ` prefix,
trailing period) and exits 2. A completely bare `ndrc` invocation (no
arguments at all) falls back to `--from-json`'s usage path and so also
exits 2.
