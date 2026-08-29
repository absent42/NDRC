/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/main.c - ndrc CLI entry point: --from-json (the DRB back end),
   --to-json (the DRF front end), and the bare invocation joining the
   two in one process.
   Copyright (C) 2026 Dan Gibson.

   JOIN (bare invocation): DRC ships drf.exe and drb.php chained by a
   JSON file on disk; this runs the two ported stages in order over an
   in-memory JSON buffer, so its stdout is the reference flow's two
   transcripts concatenated and its DDB is the flow's DDB; only its own
   argument grammar and usage/ParamError texts are NDRC's.
   PORT (--from-json): drb.php MAIN body, drb.php:1707-2135 - checks,
   header write and offset patch, two-phase emission driver, stdout
   report. The subcommand shape itself has no PHP counterpart.
   PORT (--to-json): drf.pas whole (D:/DRC/src, branch nextdaad) -
   argument parse drf.pas:320-420 (shape ported too: dot heuristic,
   case-SENSITIVE options with per-option immediate Verbose gating,
   unlike target/subtarget which are upper-cased) and CompileForTarget
   drf.pas:211-309. Every DRB Error($msg) (drb.php:1351-1355) is
   diag_fatal + return diag_exit_code; diag writes to stdout so verbose
   and map lines interleave in call order, as in the reference. One
   seam: drf.pas's 14-paragraph SYNTAX() help is a short usage line
   here, keeping SYNTAX()'s exit class 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "backend.h"
#include "diag.h"
#include "str.h"
#include "targets.h"
#include "front/constants.h"
#include "front/include.h"
#include "front/jsonexport.h"
#include "front/sintactic.h"
#include "front/symbols.h"
#include "front/tokenlist.h"

#define NDRC_VERSION "0.1"

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* PORT: file_get_contents, drb.php:1731 - reads the whole input file
   into one arena buffer for json_parse. */
static int read_whole_file(Arena *a, const char *path,
                            const unsigned char **out_data, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *buf;
    size_t got;

    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size < 0) { fclose(f); return 0; }
    rewind(f);
    buf = arena_alloc(a, (size_t)size);
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return 0;
    *out_data = buf;
    *out_len = (size_t)size;
    return 1;
}

/* One deferred option-loop message (BackendOptions.option_error):
   built into the arena rather than printed, so backend_run reports it
   at drb's own position (drb.php:1769). */
static const char *defer_option_error(Arena *a, const char *fmt,
                                      const char *arg)
{
    Str *s = str_new(a);
    str_appendf(s, fmt, arg);
    return str_cstr(s);
}

/* PORT: parseOptionalParameters' dash arm (drb.php:1366-1390), lifted
   out of run_from_json's loop so the join routes the SAME option set
   through the SAME texts. Returns 1 when `raw_arg` is a drb option
   (opts updated; opts->option_error set for a -b= out of bounds), 0
   when it matches none - the callers differ only in what they do then. */
static int drb_match_option(Arena *a, BackendOptions *opts, const char *raw_arg)
{
    char *opt_upper;

    /* NDRC extensions, case-sensitive - no DRC counterpart. --tok
       implies -auto-tokens: a tee of an unselected table says nothing. */
    if (strcmp(raw_arg, "-auto-tokens") == 0) {
        opts->auto_tokens = 1;
        return 1;
    }
    if (strcmp(raw_arg, "--tok") == 0 ||
        strncmp(raw_arg, "--tok=", 6) == 0) {
        opts->tok_tee = 1;
        opts->auto_tokens = 1;
        if (strncmp(raw_arg, "--tok=", 6) == 0)
            opts->tok_tee_path = raw_arg + 6;
        return 1;
    }

    opt_upper = str_upper_ascii(a, raw_arg);

    if (strcmp(opt_upper, "-V") == 0) {
        opts->verbose = 1;
    } else if (strcmp(opt_upper, "-C") == 0) {
        opts->forced_classic = 1;
    } else if (strcmp(opt_upper, "-CH") == 0) {
        opts->prepend_c64 = 1;
    } else if (strcmp(opt_upper, "-3H") == 0) {
        opts->prepend_plus3 = 1;
    } else if (strcmp(opt_upper, "-D") == 0) {
        /* PORT: drb.php:1373. */
        opts->forced_debug = 1;
    } else if (strcmp(opt_upper, "-NP") == 0) {
        /* PORT: drb.php:1374. */
        opts->forced_no_padding = 1;
    } else if (strcmp(opt_upper, "-P") == 0) {
        /* PORT: drb.php:1375. */
        opts->forced_padding = 1;
    } else if (strcmp(opt_upper, "-X") == 0) {
        opts->dump_to_xmb = 1;
    } else if (strncmp(opt_upper, "-B=", 3) == 0) {
        /* PORT: drb.php:1378-1384 - $value = substr($currentParam,3)
           (opt_upper is already fully upper-cased, matching
           $currentParam post-strtoupper at drb.php:1366, so the
           stripos($value,'0X')===0 case-insensitive prefix test
           reduces to a plain, case-matched "0X" compare here).
           hexdec()/intval() on the remainder, then the 1..0xFFFF
           bounds check with $currentParam (the FULL, upper-cased
           "-B=..." argument, not just its value half) echoed
           verbatim into the error text. */
        const char *value = opt_upper + 3;
        long parsed;
        if (strncmp(value, "0X", 2) == 0) {
            /* hexdec deletes ALL non-hex chars then parses the remainder
               (hexdec('1G2') == 0x12, not 0x1); input is already upper-cased so
               only 0-9/A-F need keeping. */
            const char *src = value + 2;
            char *hexbuf = arena_alloc(a, strlen(src) + 1);
            size_t hi = 0;
            for (; *src != '\0'; src++) {
                if ((*src >= '0' && *src <= '9') ||
                    (*src >= 'A' && *src <= 'F')) {
                    hexbuf[hi++] = *src;
                }
            }
            hexbuf[hi] = '\0';
            parsed = strtol(hexbuf, NULL, 16);
        } else {
            /* intval() == strtol(...,10) on every reachable input (live-pinned). */
            parsed = strtol(value, NULL, 10);
        }
        if (parsed < 1 || parsed > 0xFFFF) {
            opts->option_error = defer_option_error(
                a, "Invalid base address in %s", opt_upper);
            return 1;
        }
        opts->forced_base = parsed;
    } else {
        return 0;
    }
    return 1;
}

static int run_from_json(int argc, char **argv)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    /* Zeroed so a field this loop never reaches (option_error) can never
       be an indeterminate pointer inside backend_run. */
    BackendOptions opts = {0};
    const unsigned char *json_data = NULL;
    size_t json_len = 0;
    char *target_upper;
    const char *raw_arg;
    char *opt_upper;
    int argi;

    /* diag's stream defaults to stderr (diag.h); DRB's Error()/echo are
       one undifferentiated stdout stream, so every diag_* call here is
       redirected there too - see the file header PORT NOTE. */
    diag_set_stream(d, stdout);

    /* drb.php:1709 - the banner is unconditional, printed before any
       argument is even checked. NDRC's own text - see file header. It
       stays in this CLI layer deliberately: backend_run prints no
       banner, so a caller that drives the same back end prints exactly
       one of its own. */
    printf("NDRC %s --from-json\n", NDRC_VERSION);

    /* PORT: drb.php:1707-1728, adapted for the --from-json subcommand
       wrapper (see file header) and this task's fuller CLI shape:
       TARGET [SUBTARGET] LANG input.json [output.ddb] [options]. The
       "usage" text and its bounds checks are NDRC's own (drb.php's
       Syntax() has no analogue for a wrapped subcommand); every check
       from target_name_valid onward is the ported behaviour, and lives
       in backend_run (backend.h). */
    if (argc < 2 || strcmp(argv[1], "--from-json") != 0) {
        diag_fatal(d, "%s", BACKEND_USAGE_MSG);
        return diag_exit_code(d);
    }
    argi = 2;

    /* The positionals are consumed here but neither validated nor
       reported on - drb checks them from inside backend_run, after the
       Target line, so a missing token travels as NULL. The subtarget
       slot is consumed on the same target_takes_subtarget test drb
       validates with (drb.php:1717-1723). */
    opts.target_arg = argi < argc ? argv[argi++] : NULL;
    target_upper = opts.target_arg != NULL
                   ? str_upper_ascii(a, opts.target_arg) : NULL;
    opts.subtarget_arg = NULL;
    if (target_takes_subtarget(target_upper)) {
        opts.subtarget_arg = argi < argc ? argv[argi++] : NULL;
    }
    opts.lang_arg = argi < argc ? argv[argi++] : NULL;
    opts.input_name = argi < argc ? argv[argi++] : NULL;

    /* PORT: drb.php:1729-1731 - the file_exists gate and
       file_get_contents. Both misses are one "File not found", which
       backend_run reports when json_data arrives NULL. */
    if (opts.input_name != NULL && file_exists(opts.input_name) &&
        !read_whole_file(a, opts.input_name, &json_data, &json_len)) {
        json_data = NULL;
    }

    /* PORT: parseOptionalParameters, drb.php:1358-1397, invoked from
       drb.php:1769 - loops the remaining argv AFTER the JSON decode
       has already succeeded. A dash-prefixed argument is upper-cased
       BEFORE matching (drb.php:1366) or being echoed back in an Error
       (drb.php:1385); a non-dash argument is the output file name, kept
       in its ORIGINAL case (drb.php:1392 echoes the raw $currentParam,
       never upper-cased) - the first one wins, a second is
       "Bad parameter: <raw>". -X (dumpToXMB, drb.php:1376) is
       wired for real in backend.c; -D/-NP/-P/-B=
       (drb.php:1373-1386) are wired for real here. */
    opts.verbose = 0;
    opts.forced_classic = 0;
    opts.prepend_c64 = 0;
    opts.prepend_plus3 = 0;
    opts.dump_to_xmb = 0;
    opts.forced_padding = 0;
    opts.forced_no_padding = 0;
    opts.forced_debug = 0;
    opts.forced_base = -1;
    opts.output_path = NULL;
    opts.option_error = NULL;
    for (; argi < argc; argi++) {
        raw_arg = argv[argi];
        if (raw_arg[0] == '-') {
            if (!drb_match_option(a, &opts, raw_arg)) {
                /* PORT: drb.php:1385 - the unmatched option is echoed
                   upper-cased, exactly as it was matched. */
                opt_upper = str_upper_ascii(a, raw_arg);
                opts.option_error = defer_option_error(
                    a, "%s is not a valid option", opt_upper);
                break;
            }
            if (opts.option_error != NULL) break;
        } else if (opts.output_path == NULL) {
            opts.output_path = raw_arg;
        } else {
            opts.option_error = defer_option_error(
                a, "Bad parameter: %s", raw_arg);
            break;
        }
    }

    return backend_run(a, d, json_data, json_len, &opts);
}

/* ===================================================================
   --to-json: the DRF front-end CLI (drf.pas WHOLE)
   =================================================================== */

/* NDRC's own usage text, standing in for drf.pas's 14-paragraph
   SYNTAX() (drf.pas:11-43) - see the file header PORT NOTE. Printed
   only when too few positional arguments are present to identify a
   target and an input file at all (drf.pas:327's ParamCount()<2
   condition); every OTHER missing/invalid argument below falls through
   to a real, byte-exact ParamError (drf.pas's ParamStr returns '' past
   the argument list, which is what lets those later checks fire their
   own specific messages instead of ever reaching here). */
static const char TO_JSON_USAGE_MSG[] =
    "usage: ndrc --to-json TARGET [SUBTARGET] file.dsf [output.json] "
    "[symbols] [options]";

/* PORT: ParamStr(N) (drf.pas). `args` is argv shifted so args[1] is
   the first positional - argv+1 for a mode-flagged invocation
   (argv[1]="--to-json" standing in for the Pascal program's own
   ParamStr(0)), argv itself for the bare join. Pascal's ParamStr
   returns an empty string for any index past ParamCount() rather than
   erroring - this is why drf.pas's own argument checks are almost all
   ParamError, not a bounds crash: a missing subtarget, input file or
   output name all flow through here as "" and hit their own specific
   ParamError text downstream. i is 1-based, matching Pascal
   ParamStr/ParamCount. */
static const char *pstr(char **args, int argc_eff, int i)
{
    if (i < 1 || i > argc_eff) return "";
    return args[i];
}

/* PORT: SysUtils.ChangeFileExt, as drf.pas itself uses it (drf.pas:224,
   348) - NOT drb.php's pathinfo()-based replace_extension in backend.c (that
   one's dirname-prefix quirk is PHP-specific and has no counterpart
   here). Replaces everything from the LAST '.' after the last path
   separator with new_ext (which must carry its own leading '.'); if no
   such dot exists, appends new_ext. The directory part, if any, is
   left untouched - no "./" is ever inserted for a bare filename. */
static char *change_file_ext(Arena *a, const char *filename, const char *new_ext)
{
    size_t len = strlen(filename);
    size_t i;
    long dot = -1, sep = -1;
    Str *out;

    for (i = 0; i < len; i++) {
        if (filename[i] == '.') dot = (long)i;
        if (filename[i] == '/' || filename[i] == '\\') sep = (long)i;
    }
    if (dot <= sep) dot = -1;

    out = str_new(a);
    if (dot >= 0) str_append_n(out, filename, (size_t)dot);
    else str_append(out, filename);
    str_append(out, new_ext);
    return (char *)str_cstr(out);
}

/* PORT: CheckEND (drf.pas:91-108). Opens InputFileName RAW (before
   Preparse ever runs, so an /END living only inside a #include'd file
   is invisible here - the check's own stated intent, drf.pas:419's
   error text). Trims each line (FPC Trim: strips bytes <= 0x20 at both
   ends) and looks for a line equal to exactly "/END". PORT NOTE
   (simplification, not a catalogued defect): reads with ordinary text-
   mode fgets rather than lexlib.c's byte-exact FPC ReadLn emulation -
   this check is a boolean existence scan for a short literal marker,
   not a position-reporting or content-sensitive path, so the CRLF/LF/
   lone-CR edge cases section 15.2 documents for the lexer do not carry
   the same fidelity weight here. */
static int check_end(const char *filename)
{
    FILE *f = fopen(filename, "r");
    char line[1024];

    if (f == NULL) return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        size_t len = strlen(line);
        size_t start = 0;
        while (len > 0 && (unsigned char)line[len - 1] <= ' ') len--;
        while (start < len && (unsigned char)line[start] <= ' ') start++;
        if (len - start == 4 && strncmp(line + start, "/END", 4) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* PORT: isValidSubTarget (drf.pas:311-317), the four real branches -
   19.20's "undefined Result" leg is DEAD (the sole call site below
   already restricts `target` to these same four names first) and is
   not reproduced; this returns a real, defined 0 for any other target,
   which is what the reference's call site behaviour amounts to anyway. */
static int to_json_valid_subtarget(const char *target, const char *sub)
{
    if (strcmp(target, "MSX2") == 0) {
        return strcmp(sub, "5_6") == 0 || strcmp(sub, "5_8") == 0 ||
               strcmp(sub, "6_6") == 0 || strcmp(sub, "6_8") == 0 ||
               strcmp(sub, "7_6") == 0 || strcmp(sub, "7_8") == 0 ||
               strcmp(sub, "8_6") == 0 || strcmp(sub, "8_8") == 0 ||
               strcmp(sub, "10_6") == 0 || strcmp(sub, "10_8") == 0 ||
               strcmp(sub, "12_6") == 0 || strcmp(sub, "12_8") == 0;
    }
    if (strcmp(target, "PC") == 0) {
        return strcmp(sub, "VGA256") == 0 || strcmp(sub, "VGA") == 0 ||
               strcmp(sub, "EGA") == 0 || strcmp(sub, "CGA") == 0 ||
               strcmp(sub, "TEXT") == 0;
    }
    if (strcmp(target, "ZX") == 0) {
        return strcmp(sub, "PLUS3") == 0 || strcmp(sub, "ESXDOS") == 0 ||
               strcmp(sub, "NEXT") == 0 || strcmp(sub, "UNO") == 0 ||
               strcmp(sub, "48K") == 0 || strcmp(sub, "128K") == 0;
    }
    if (strcmp(target, "ZX81") == 0) {
        return strcmp(sub, "16K") == 0 || strcmp(sub, "SD81B") == 0;
    }
    return 0;
}

/* PORT: getPCColsBySubtarget/getMSX2ColsBySubtarget/getZX81ColsBySubtarget
   + getColsByTarget (drf.pas:62-128). Every branch falls
   through to a fixed default rather than failing - this is defect
   19.3's mechanism (the target is never validated - see
   to_json_symbol_target_upper below) reproduced deliberately: an
   unrecognised target silently gets COLS=42/ROWS=25 rather than being
   rejected. */
static int to_json_pc_cols(const char *sub)
{
    if (strcmp(sub, "TEXT") == 0) return 80;
    if (strcmp(sub, "VGA256") == 0) return 53;
    return 53; /* "Conservative" - VGA/EGA/CGA and anything else */
}

static int to_json_msx2_cols(const char *sub)
{
    if (strcmp(sub, "5_6") == 0) return 42;
    if (strcmp(sub, "5_8") == 0) return 32;
    if (strcmp(sub, "6_6") == 0) return 85;
    if (strcmp(sub, "6_8") == 0) return 64;
    if (strcmp(sub, "7_6") == 0) return 85;
    if (strcmp(sub, "7_8") == 0) return 64;
    if (strcmp(sub, "8_6") == 0) return 42;
    if (strcmp(sub, "8_8") == 0) return 32;
    if (strcmp(sub, "10_6") == 0) return 42;
    if (strcmp(sub, "10_8") == 0) return 32;
    if (strcmp(sub, "12_6") == 0) return 42;
    if (strcmp(sub, "12_8") == 0) return 32;
    return 42; /* "Conservative" */
}

static int to_json_zx81_cols(const char *sub)
{
    if (strcmp(sub, "16K") == 0) return 32;
    return 42;
}

static int to_json_cols_for_target(const char *target, const char *sub)
{
    if (strcmp(target, "PC") == 0) return to_json_pc_cols(sub);
    if (strcmp(target, "ZX") == 0) return 42;
    if (strcmp(target, "C64") == 0) return 40;
    if (strcmp(target, "CP4") == 0) return 40;
    if (strcmp(target, "CPC") == 0) return 40;
    if (strcmp(target, "CPM") == 0) return 80;
    if (strcmp(target, "HTML") == 0) return 53;
    if (strcmp(target, "MSX") == 0) return 42;
    if (strcmp(target, "MSX2") == 0) return to_json_msx2_cols(sub);
    if (strcmp(target, "ST") == 0) return 53;
    if (strcmp(target, "AMIGA") == 0) return 53;
    if (strcmp(target, "PCW") == 0) return 90;
    if (strcmp(target, "ZX81") == 0) return to_json_zx81_cols(sub);
    if (strcmp(target, "NEXTDAAD") == 0) return 80;
    return 42; /* "Conservative" - any unrecognised target, per 19.3 */
}

/* PORT: GetRowsByTarget (drf.pas:130-139). */
static int to_json_rows_for_target(const char *target)
{
    if (strcmp(target, "PCW") == 0) return 32;
    if (strcmp(target, "MSX2") == 0) return 26;
    if (strcmp(target, "ZX81") == 0) return 24;
    if (strcmp(target, "ZX") == 0) return 24;
    if (strcmp(target, "MSX") == 0) return 24;
    if (strcmp(target, "NEXTDAAD") == 0) return 32;
    return 25;
}

/* PORT: the BIT8/BIT16 membership tests inside CompileForTarget
   (drf.pas:243-245). `machine` is already upper-cased (AnsiUpperCase of
   the already-upper-cased Target - idempotent). */
static int to_json_is_bit8_target(const char *machine)
{
    return strcmp(machine, "ZX") == 0 || strcmp(machine, "CPC") == 0 ||
           strcmp(machine, "PCW") == 0 || strcmp(machine, "MSX") == 0 ||
           strcmp(machine, "C64") == 0 || strcmp(machine, "CP4") == 0 ||
           strcmp(machine, "MSX2") == 0 || strcmp(machine, "ZX81") == 0 ||
           strcmp(machine, "CPM") == 0 || strcmp(machine, "NEXTDAAD") == 0;
}

static int to_json_is_bit16_target(const char *machine)
{
    return strcmp(machine, "PC") == 0 || strcmp(machine, "AMIGA") == 0 ||
           strcmp(machine, "ST") == 0;
}

/* PORT: the trailing "additional symbols" comma split (drf.pas:292-300,
   `ExtractWord(i, AdditionalSymbols, [','])`).
   Live-verified against drf.exe 2026-08-27: "p3,,p4" (a doubled
   separator) adds P3=1 and P4=2, NOT P3=1/P4=3 - so ExtractWord treats
   a RUN of separators as one boundary and never yields an empty word;
   this tokenizer does the same (skip separator runs, no empty tokens),
   assigning each token its own 1-based position as its value. Each
   token is passed to symbols_add RAW (untouched) - symbols_add folds
   through str_upper_latin1 itself (str.h), which is the whole point of
   this path: an argv symbol carrying a cp1252 accented byte gets the
   SAME fold a #define'd DSF identifier would (str.h's own PROVENANCE
   comment: this exact CLI path is what its live-probe measured). */
static void inject_additional_symbols(SymbolList *syms, Arena *a, Diag *d,
                                       const char *s)
{
    long i = 1;
    const char *p = s;

    while (*p != '\0') {
        const char *start;
        size_t len;
        char *word;

        while (*p == ',') p++;
        if (*p == '\0') break;
        start = p;
        while (*p != '\0' && *p != ',') p++;
        len = (size_t)(p - start);
        word = arena_strndup(a, start, len);
        symbols_add(syms, a, d, word, i);
        i++;
    }
}

/* PORT: the built-in symbol block inside CompileForTarget
   (drf.pas:233-289), IN ORDER (#ifdef in an earlier section only sees
   what is already defined, so order is observable). Item 11 (additional
   symbols) is injected separately by the caller, immediately after
   this returns, since it depends on the CLI's trailing argument rather
   than target/subtarget. */
static void inject_builtin_symbols(SymbolList *syms, Arena *a, Diag *d,
                                    const char *target, const char *subtarget,
                                    int v3code)
{
    char *machine;
    int cols;

    /* 1: the target itself. */
    symbols_add(syms, a, d, target, 1);

    /* 2: MODE_<sub> BEFORE the bare subtarget, only when one was given. */
    if (subtarget != NULL && subtarget[0] != '\0') {
        Str *mode = str_new(a);
        str_append(mode, "MODE_");
        str_append(mode, subtarget);
        symbols_add(syms, a, d, str_cstr(mode), 1);
        symbols_add(syms, a, d, subtarget, 1);
    }

    /* 3: BIT8/BIT16 (AnsiUpperCase again - idempotent, target is
       already upper-cased by the caller). */
    machine = str_upper_ascii(a, target);
    if (to_json_is_bit8_target(machine)) symbols_add(syms, a, d, "BIT8", 1);
    if (to_json_is_bit16_target(machine)) symbols_add(syms, a, d, "BIT16", 1);

    /* 4: COLS, guarded on cols<>0 - dead in the current table (every
       branch returns non-zero) but ported structurally. */
    cols = to_json_cols_for_target(target, subtarget != NULL ? subtarget : "");
    if (cols != 0) symbols_add(syms, a, d, "COLS", cols);

    /* 5: ROWS, unconditional. */
    symbols_add(syms, a, d, "ROWS", to_json_rows_for_target(target));

    /* 6: CARRIED/NOT_CREATED/NON_CREATED/WORN/HERE, then HERE AGAIN -
       the second call is a verbatim no-op (symbols_add's duplicate
       rejection), reproduced rather than dropped. */
    symbols_add(syms, a, d, "CARRIED", LOC_CARRIED);
    symbols_add(syms, a, d, "NOT_CREATED", LOC_NOT_CREATED);
    symbols_add(syms, a, d, "NON_CREATED", LOC_NOT_CREATED);
    symbols_add(syms, a, d, "WORN", LOC_WORN);
    symbols_add(syms, a, d, "HERE", LOC_HERE);
    symbols_add(syms, a, d, "HERE", LOC_HERE);

    /* drf.pas:259-262 reads Now once per AddSymbol (4 reads, not cached) -
       replicated; a run straddling midnight could in theory see two
       different dates across the four symbols. The MOD 100 no-op is
       kept verbatim. */
    {
        time_t t1 = time(NULL);
        struct tm tm1 = *localtime(&t1);
        symbols_add(syms, a, d, "YEARHIGH", (long)(tm1.tm_year + 1900) / 100);
    }
    {
        time_t t2 = time(NULL);
        struct tm tm2 = *localtime(&t2);
        symbols_add(syms, a, d, "YEARLOW", (long)(tm2.tm_year + 1900) % 100);
    }
    {
        time_t t3 = time(NULL);
        struct tm tm3 = *localtime(&t3);
        symbols_add(syms, a, d, "MONTH", (long)(tm3.tm_mon + 1) % 100);
    }
    {
        time_t t4 = time(NULL);
        struct tm tm4 = *localtime(&t4);
        symbols_add(syms, a, d, "DAY", (long)tm4.tm_mday % 100);
    }

    /* 8: the SFX command symbols. */
    symbols_add(syms, a, d, "PLAYSFX", 1);
    symbols_add(syms, a, d, "PLAYSFXL", 2);
    symbols_add(syms, a, d, "PLAYSFXF", 3);
    symbols_add(syms, a, d, "PLAYSFXFL", 4);
    symbols_add(syms, a, d, "STOPSFX", 5);
    symbols_add(syms, a, d, "PLAYDRO", 6);
    symbols_add(syms, a, d, "PLAYDROL", 7);
    symbols_add(syms, a, d, "STOPDRO", 8);
    symbols_add(syms, a, d, "PLAYFLI", 9);
    symbols_add(syms, a, d, "PLAYFLIL", 10);

    /* 9: the MOUSE command symbols. */
    symbols_add(syms, a, d, "RESETMS", 0);
    symbols_add(syms, a, d, "SHOWMS", 1);
    symbols_add(syms, a, d, "HIDEMS", 2);
    symbols_add(syms, a, d, "GETMS", 3);
    symbols_add(syms, a, d, "GETFINEMS", 4);
    symbols_add(syms, a, d, "POINTERMS", 5);
    symbols_add(syms, a, d, "DELTAXMS", 6);
    symbols_add(syms, a, d, "DELTAYMS", 7);

    /* 10: exactly one of V3/V2. MAX_PARAM_ACCEPTING_INDIRECTION is NOT
       set here - sintactic.c derives it from FrontOptions.v3 at its own
       use site (task-6-report.md's interface note). */
    if (v3code) symbols_add(syms, a, d, "V3", 1);
    else symbols_add(syms, a, d, "V2", 1);
}

/* NDRC's own usage text for the bare (join) invocation - the same
   stand-in role TO_JSON_USAGE_MSG plays for drf.pas's SYNTAX(), for a
   CLI shape that has no reference at all (DRC ships two programs). */
static const char JOIN_USAGE_MSG[] =
    "usage: ndrc TARGET [SUBTARGET] LANG file.dsf [output.ddb] "
    "[symbols] [options]";

/* The parsed drf-side command line: drf.pas's own main-block locals,
   collected so --to-json and the join can share one parser. */
typedef struct {
    char *target_upper;         /* drf.pas:328, never validated (19.3) */
    char *subtarget_upper;      /* NULL when the target takes none */
    const char *input_path;
    const char *output_name;    /* the dot-heuristic slot; NULL = unclaimed */
    const char *additional_symbols;
    int verbose, no_semantic, semantic_warnings;
    int force_normal_messages, force_x_messages;
    int check_maluva, v3code, ascii7, replace_xcondacts;
    int json_tee;               /* join only: --json seen */
    const char *json_path;      /* join only: --json=path, NULL = default */
} FrontCli;

static void front_cli_init(FrontCli *fc)
{
    memset(fc, 0, sizeof *fc);
    fc->check_maluva = 1;       /* drf.pas's own default */
    fc->additional_symbols = "";
}

static int join_is_json_opt(const char *arg)
{
    return strcmp(arg, "--json") == 0 || strncmp(arg, "--json=", 7) == 0;
}

static int join_is_tok_opt(const char *arg)
{
    return strcmp(arg, "--tok") == 0 || strncmp(arg, "--tok=", 6) == 0;
}

/* The join's own options: --json[=path] first (join-specific), then the
   drb set through drb_match_option so the join and --from-json share
   one matcher and one set of texts. Returns 1 when routed. */
static int join_match_option(Arena *a, FrontCli *fc, BackendOptions *bopts,
                              const char *arg)
{
    if (join_is_json_opt(arg)) {
        fc->json_tee = 1;
        fc->json_path = strncmp(arg, "--json=", 7) == 0 ? arg + 7 : NULL;
        return 1;
    }
    if (bopts->option_error != NULL) {
        /* drb's own loop BREAKS at its first option error (drb.php:
           1385-1389), so nothing after it is applied and the FIRST bad
           option is the one reported - which is what --from-json does.
           Matched against a throwaway copy so the token is still
           recognised (not a ParamError) but changes nothing. */
        BackendOptions ignored = *bopts;
        return drb_match_option(a, &ignored, arg);
    }
    return drb_match_option(a, bopts, arg);
}

/* PORT: drf.pas:328-413 - the whole positional/option grammar in one
   place: the never-validated upper-cased target, the subtarget
   MSX2/PC/ZX/ZX81 demand, the input-file check, the 19.4 dot heuristic
   and the options/symbol-list loop with its per-option 19.19 verbose
   gating. Shared so the grammar cannot drift between modes. `bopts`
   non-NULL is the join: a LANG positional is consumed after the
   subtarget slot, --json is accepted, and the drb option set is routed
   into `bopts` rather than rejected. Returns 0, or the exit code of the
   first ParamError. */
static int parse_front_cli(Arena *a, Diag *d, char **args, int argc_eff,
                            FrontCli *fc, BackendOptions *bopts)
{
    int next_param;
    const char *aux;

    /* PORT: drf.pas:328 - Target := UpperCase(ParamStr(1)), NEVER
       validated (defect 19.3, REACHABLE-BUG - reproduced deliberately:
       an unrecognised target compiles silently at COLS=42/ROWS=25 and,
       in the join, dies at the drb stage exactly as the reference flow
       does). */
    fc->target_upper = str_upper_ascii(a, pstr(args, argc_eff, 1));
    if (bopts != NULL) bopts->target_arg = pstr(args, argc_eff, 1);
    next_param = 2;

    /* PORT: drf.pas:330-336. */
    if (strcmp(fc->target_upper, "MSX2") == 0 ||
        strcmp(fc->target_upper, "PC") == 0 ||
        strcmp(fc->target_upper, "ZX") == 0 ||
        strcmp(fc->target_upper, "ZX81") == 0) {
        const char *subtarget_raw = pstr(args, argc_eff, next_param);
        fc->subtarget_upper = str_upper_ascii(a, subtarget_raw);
        if (bopts != NULL) bopts->subtarget_arg = subtarget_raw;
        if (!to_json_valid_subtarget(fc->target_upper, fc->subtarget_upper)) {
            /* drf.pas's literal already ends in '.'; ParamError appends another -
               the doubled period is reference behaviour (live-verified raw bytes). */
            diag_param_error(d,
                "\"%s\" is not a valid subtarget for target \"%s\". "
                "Please specify a valid subtarget. Call DRF without "
                "parameters for more information.",
                fc->subtarget_upper, fc->target_upper);
            return diag_exit_code(d);
        }
        next_param++;
    }

    /* The join's LANG slot: consumed unvalidated, since validity is the
       drb stage's business and its error belongs at the drb stage's own
       transcript position (backend.c). */
    if (bopts != NULL) {
        bopts->lang_arg = pstr(args, argc_eff, next_param);
        next_param++;
    }

    /* PORT: drf.pas:337-339. */
    fc->input_path = pstr(args, argc_eff, next_param);
    if (!file_exists(fc->input_path)) {
        diag_param_error(d, "Input file not found: \"%s\"", fc->input_path);
        return diag_exit_code(d);
    }
    next_param++;

    /* PORT: the 19.4 output-name/symbol-list heuristic (drf.pas:340-349)
       - a dot anywhere in the next argument makes it the
       output name (consuming the slot); no dot leaves next_param
       UNCHANGED, so the very same argument is re-read by the loop below
       as an option or the symbol list. Live-pinned examples (drf.exe,
       2026-08-27, both against the same probe.dsf/NEXTDAAD run): a
       dotless trailing argument ("mysymbol") is added as a symbol and
       the default "<input>.json" name is used; a dotted one ("my.out")
       is used verbatim as the output path with no extension massaging.
       In the join the claimed slot is the DDB name the drb stage would
       have been given, the JSON name staying the flow's own default.

       The heuristic tests for a dot ONLY - a dashed argument carrying
       one is claimed too (live-verified: `drf.exe NEXTDAAD g.dsf -v3.5`
       writes a file literally named "-v3.5"). The join's own
       `--json=<path>` and `--tok=<path>` are the carve-outs from that:
       both always carry a dot, and letting either become the DDB name
       would make its tee silently redirect the compile it is supposed
       to leave untouched. --to-json (bopts NULL) keeps the heuristic
       exactly as drf.pas has it. The carve-out only declines the slot: a
       `--json=x.json out.ddb` pair leaves the slot unclaimed, so
       out.ddb reaches the loop below as the symbol list - drf's own
       grammar, where the heuristic looks at the FIRST post-input
       argument only. */
    aux = pstr(args, argc_eff, next_param);
    if (strchr(aux, '.') != NULL &&
        !(bopts != NULL && (join_is_json_opt(aux) || join_is_tok_opt(aux)))) {
        fc->output_name = aux;
        next_param++;
    }

    /* PORT: drf.pas:350-413 - the options/symbol-list loop. Case-
       SENSITIVE option matching (no UpperCase, unlike target/subtarget);
       only the LAST non-option argument
       survives into AdditionalSymbols (no accumulation); each verbose-
       gated line is gated on Verbose's value AT THAT MOMENT in this
       SAME left-to-right pass (defect 19.19), not its final value. */
    for (; next_param <= argc_eff; next_param++) {
        const char *arg = pstr(args, argc_eff, next_param);

        if (arg[0] != '-') {
            fc->additional_symbols = arg;
            continue;
        }
        if (strcmp(arg, "-verbose") == 0) {
            fc->verbose = 1;
            printf("Verbose mode ON\n");
        } else if (strcmp(arg, "-no-semantic") == 0) {
            fc->no_semantic = 1;
            if (fc->verbose) printf("Warning: DRF won't make semantic analysis\n");
        } else if (strcmp(arg, "-semantic-warnings") == 0) {
            fc->semantic_warnings = 1;
            if (fc->verbose) {
                printf("Warning: Semantic analysys errors will just "
                       "generate warnings\n");
            }
        } else if (strcmp(arg, "-force-normal-messages") == 0) {
            fc->force_normal_messages = 1;
            if (fc->verbose) printf("Warning: Forced Normal Messages\n");
        } else if (strcmp(arg, "-force-x-messages") == 0) {
            fc->force_x_messages = 1;
            if (fc->verbose) printf("Warning: Forced XMessages\n");
        } else if (strcmp(arg, "-check-maluva-disabled") == 0) {
            fc->check_maluva = 0;
            /* PORT NOTE (defect 19.18, verbatim): this option's own
               verbose confirmation is copy-pasted from -force-x-messages
               above - "Forced XMessages", not anything naming Maluva. */
            if (fc->verbose) printf("Warning: Forced XMessages\n");
        } else if (strcmp(arg, "-v3") == 0) {
            fc->v3code = 1;
            printf("Generating DAAD V3 DDB\n");
        } else if (strcmp(arg, "-7") == 0) {
            fc->ascii7 = 1;
            printf("Generating DAAD 7-bit ASCII DDB\n");
        } else if (strcmp(arg, "-replace-xcondacts") == 0) {
            fc->replace_xcondacts = 1;
            if (fc->verbose) printf("Warning: Replacing Xcondacts\n");
        } else if (bopts != NULL && join_match_option(a, fc, bopts, arg)) {
            /* A drb-stage option or the --json tee: no drf-stage echo -
               drb's own option echoes belong at the drb stage's
               position, which backend_run prints them at. */
        } else {
            diag_param_error(d, "Invalid option: %s", arg);
            return diag_exit_code(d);
        }
    }
    return 0;
}

/* One buffer, one file. Carries jsonexport_write's own two failure
   texts because the bytes written here are THE rendered buffer the
   join's back end consumes - delegating would render a second time. */
static int write_file_bytes(Diag *d, const char *path,
                             const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        diag_fatal(d, "cannot open \"%s\" for writing", path);
        return diag_exit_code(d);
    }
    fwrite(data, 1, len, f);
    if (fclose(f) != 0) {
        diag_fatal(d, "error writing \"%s\"", path);
        return diag_exit_code(d);
    }
    return 0;
}

/* The whole drf stage from a parsed command line onward: drf.pas's two
   cross-option checks (415-416), CheckEND (419) and CompileForTarget
   (211-309), with drf.pas's own stdout for all of it. `json_name` is
   the name the Generating/generated lines echo - the flow's JSON name;
   `write_path` is where the rendered bytes land, or NULL to keep them
   in memory only (the join without --json). On success *out_data and
   *out_len hold the document, owned by `a`. */
static int front_compile(Arena *a, Diag *d, const FrontCli *fc,
                          const char *json_name, const char *write_path,
                          const unsigned char **out_data, size_t *out_len)
{
    char *temp_path;
    Token *stream;
    FrontOptions opts;
    int rc;

    /* PORT: drf.pas:415-416. */
    /* Doubled period is reference behaviour - see the ParamError note above. */
    if (fc->no_semantic && fc->semantic_warnings) {
        diag_param_error(d,
            "You can't avoid semantic checking and at the same time "
            "expect semantic warnings.");
        return diag_exit_code(d);
    }
    if (fc->force_normal_messages && fc->force_x_messages) {
        diag_param_error(d,
            "You can't force XMesages and normal messages at the same time.");
        return diag_exit_code(d);
    }

    /* PORT: drf.pas:419 - scans the RAW input file, before Preparse.
       Doubled period is reference behaviour - see the ParamError note above. */
    if (!check_end(fc->input_path)) {
        diag_param_error(d,
            "Input file has no /END section. Please make sure /END it's "
            "in main file, not in #include files, if any.");
        return diag_exit_code(d);
    }

    /* ---- CompileForTarget (drf.pas:211-309) ---- */

    /* Downstream verbose lines (Preparse's "Including...", drf.pas:178)
       read Verbose's FINAL value after the options loop completes
       (drf.pas:217) - not the per-option immediate reads at drf.pas:359-408. */
    diag_set_verbose(d, fc->verbose);

    if (fc->verbose) {
        printf("Target: %s", fc->target_upper);
        if (fc->subtarget_upper != NULL) {
            printf(" | Subtarget:%s", fc->subtarget_upper);
        }
        printf("\n");
    }
    printf("Reading %s\n", fc->input_path);

    temp_path = change_file_ext(a, fc->input_path, ".___");

    sintactic_init(a, d);

    /* PIPELINE ORDER: Preparse -> Lex -> symbol injection (14.1 order)
       -> Sintactic -> FixForwardLabels -> Generate -> temp-file delete -
       matching CompileForTarget's own statement order exactly
       (drf.pas:225-308). */
    if (!preparse(a, d, fc->input_path, temp_path)) return diag_exit_code(d);

    {
        int errs_before = diag_error_count(d);
        stream = lex_tokenize(a, d, temp_path);
        if (stream == NULL && diag_error_count(d) > errs_before) {
            return diag_exit_code(d);
        }
    }

    inject_builtin_symbols(sintactic_symbols(), a, d, fc->target_upper,
                            fc->subtarget_upper, fc->v3code);
    if (fc->additional_symbols[0] != '\0') {
        inject_additional_symbols(sintactic_symbols(), a, d,
                                  fc->additional_symbols);
    }

    opts.target = fc->target_upper;
    opts.subtarget = fc->subtarget_upper != NULL ? fc->subtarget_upper : "";
    opts.v3 = fc->v3code;
    opts.verbose = fc->verbose;
    opts.ascii7 = fc->ascii7;
    opts.no_semantic = fc->no_semantic;
    opts.semantic_warnings = fc->semantic_warnings;
    opts.force_normal_messages = fc->force_normal_messages;
    opts.force_x_messages = fc->force_x_messages;
    opts.check_maluva = fc->check_maluva;
    opts.replace_xcondacts = fc->replace_xcondacts;

    printf("Checking Syntax...\n");
    rc = sintactic_parse(a, d, stream, &opts);
    if (rc != 0) return rc;

    printf("Updating forward references...\n");
    rc = sintactic_fix_forward_labels();
    if (rc != 0) return rc;

    /* PORT: drf.pas:305-306 - one combined Write+WriteLn pair. */
    printf("Generating %s [Classic mode %s]\n", json_name,
           sintactic_classic_mode() ? "ON" : "OFF");

    /* PORT: GenerateOutput -> GenerateJSON (UCodeGeneration.pas:18-21
       fold: GenerateOutput is a parameterless-in-substance pass-through,
       its Target parameter never read). Rendered rather than written
       straight out so the join can feed the SAME bytes to its back end
       and, with --json, tee them; --to-json writes them and nothing
       else. The trailing "<file> generated." line (UJSONExport.pas:494,
       GenerateJSON's own last statement, AFTER Close(JSON)) is printed
       here, at the same stdout position. */
    rc = jsonexport_render(a, d, &opts, out_data, out_len);
    if (rc != 0) return rc;
    if (write_path != NULL) {
        rc = write_file_bytes(d, write_path, *out_data, *out_len);
        if (rc != 0) return rc;
    }
    printf("%s generated.\n", json_name);

    /* PORT: drf.pas:308 - DeleteFile(TempFileName), the pipeline's
       final stage, reached only after a fully successful compile: every
       earlier `return` above (options, Preparse, Lex, Sintactic,
       FixForwardLabels, GenerateOutput) leaves the temp file on disk,
       matching the reference - Halt() terminates the Pascal process
       immediately at each of those sites, so DeleteFile never runs
       either. */
    remove(temp_path);

    return 0;
}

static int run_to_json(int argc, char **argv)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    int argc_eff = argc - 2;
    FrontCli fc;
    const char *output_path;
    const unsigned char *data = NULL;   /* rendered here, unused: the
                                           file IS this mode's output */
    size_t len = 0;
    int rc;

    diag_set_stream(d, stdout);

    /* drf.pas's own banner (drf.pas:323-325) is DAAD Reborn Compiler
       Frontend's own product identity, not NDRC's - NDRC prints its
       own, matching --from-json's precedent above (this is the ONE
       stripped line the oracle gate skips on each side). */
    printf("NDRC %s --to-json\n", NDRC_VERSION);

    /* PORT: drf.pas:327 `IF (ParamCount()<2) THEN SYNTAX();` - see the
       file header PORT NOTE for why the giant SYNTAX() text is not
       reproduced here, and why this keeps SYNTAX()'s exit class 1
       regardless (drf.pas:42's Halt(1) inside SYNTAX(), distinct from
       ParamError's Halt(2) at drf.pas:53). */
    if (argc_eff < 2) {
        printf("%s\n", TO_JSON_USAGE_MSG);
        return 1;
    }

    front_cli_init(&fc);
    rc = parse_front_cli(a, d, argv + 1, argc_eff, &fc, NULL);
    if (rc != 0) return rc;

    /* PORT: drf.pas:348 - an unclaimed output slot defaults to
       ChangeFileExt(InputFileName, '.json'). */
    output_path = fc.output_name != NULL
                  ? fc.output_name
                  : change_file_ext(a, fc.input_path, ".json");

    return front_compile(a, d, &fc, output_path, output_path, &data, &len);
}

/* ===================================================================
   The join: one process, drf stage then drb stage, no JSON file
   =================================================================== */

static int run_join(int argc, char **argv)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    int argc_eff = argc - 1;
    FrontCli fc;
    /* Zeroed so a field this CLI never sets (option_error) can never be
       an indeterminate pointer inside backend_run. */
    BackendOptions bopts = {0};
    const char *json_name;
    const unsigned char *data = NULL;
    size_t len = 0;
    int rc;

    /* Both stages print through one diag on one stream, in call order -
       the drb half depends on it (see run_from_json). */
    diag_set_stream(d, stdout);

    /* One banner for the whole flow, printed first; neither stage
       prints one of its own. */
    printf("NDRC %s\n", NDRC_VERSION);

    /* Too few positionals to name a target, a language and an input:
       ndrc-owned (a joined CLI has no reference), taking --to-json's
       usage class - usage text, exit 1, not ParamError's 2. */
    if (argc_eff < 3) {
        printf("%s\n", JOIN_USAGE_MSG);
        return 1;
    }

    bopts.forced_base = -1;     /* unset, per backend.h */
    front_cli_init(&fc);
    rc = parse_front_cli(a, d, argv, argc_eff, &fc, &bopts);
    if (rc != 0) return rc;

    /* The flow's JSON name: drf's own default ChangeFileExt naming,
       which is what the reference flow hands drb. It drives the
       drf-stage echoes, the drb stage's default DDB name and its .tok
       candidate (measured: `drb.php NEXTDAAD EN TOKFILE.json` loads
       ".\TOKFILE.tok"). A --json=path tee never moves it. */
    json_name = change_file_ext(a, fc.input_path, ".json");

    rc = front_compile(a, d, &fc, json_name,
                       fc.json_tee
                       ? (fc.json_path != NULL ? fc.json_path : json_name)
                       : NULL,
                       &data, &len);
    if (rc != 0) return rc;

    bopts.input_name = json_name;
    bopts.output_path = fc.output_name;
    return backend_run(a, d, data, len, &bopts);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--to-json") == 0) {
        return run_to_json(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "--from-json") == 0) {
        return run_from_json(argc, argv);
    }
    /* Anything else with an argument is the join. With no arguments at
       all the pre-join behaviour stands: run_from_json's usage error. */
    if (argc >= 2) {
        return run_join(argc, argv);
    }
    return run_from_json(argc, argv);
}
