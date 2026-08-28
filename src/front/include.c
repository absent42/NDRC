/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/include.c - Copyright (C) 2026 Dan Gibson.

   PORT: UInclude.pas + drf.pas's Preparse (drf.pas:154-208). See
   include.h for the surface contract and the PORT NOTEs. */
#include "include.h"

#include <stdio.h>
#include <string.h>

#include "str.h"

/* PORT: UInclude.pas's IncludeList unit global, mirrored as file-scope
   statics; arena-backed, process lifetime. */
static IncludeData *g_map;
static size_t g_len;
static size_t g_cap;

void include_reset(void)
{
    g_map = NULL;
    g_len = 0;
    g_cap = 0;
}

void include_add_line(Arena *a, long main_line,
                       const char *original_filename, long original_line)
{
    /* PORT: AddLine (UInclude.pas:26-30) - SetLength(IncludeList,
       AMainLine); IncludeList[AMainLine-1] := IncludeData. Preparse
       only ever passes main_line = g_len + 1; anything else is a
       programming error in this port (the Pascal would silently
       truncate or leave holes - no caller exists that does). */
    if (main_line != (long)g_len + 1) {
        fprintf(stderr,
                "ndrc: internal error, include_add_line called with "
                "main_line %ld, expected %ld\n", main_line, (long)g_len + 1);
        return;
    }
    if (g_len == g_cap) {
        size_t new_cap = g_cap ? g_cap * 2 : 64;
        IncludeData *grown = arena_alloc(a, sizeof(IncludeData) * new_cap);
        if (g_len) memcpy(grown, g_map, sizeof(IncludeData) * g_len);
        g_map = grown;
        g_cap = new_cap;
    }
    g_map[g_len].original_filename = arena_strdup(a, original_filename);
    g_map[g_len].original_line = original_line;
    g_len++;
}

int include_get_data(long main_line, IncludeData *out)
{
    /* PORT: GetIncludeData (UInclude.pas:32-35), with the bounds
       guard include.h documents (the reference reads out of range
       with no check). */
    if (main_line < 1 || (size_t)main_line > g_len) return 0;
    *out = g_map[main_line - 1];
    return 1;
}

long include_remap_for_diag(Diag *d, long line)
{
    IncludeData inc;
    if (include_get_data(line, &inc)) {
        diag_set_source(d, inc.original_filename);
        return inc.original_line;
    }
    return line;
}

/* Mirrors FPC's ReadLn (CRLF/LF collapse, bare CR survives, same as
   lexlib.c's fetch_line). Returns 0 only at end-of-file. Growable Str,
   unlike lexlib's fixed 8192-byte buffer. */
static int read_line(FILE *f, Str *line)
{
    int c;

    str_clear(line);
    c = fgetc(f);
    if (c == EOF) return 0;
    for (;;) {
        if (c == EOF) break;
        if (c == '\n') {
            size_t n = str_len(line);
            if (n > 0 && str_bytes(line)[n - 1] == '\r') {
                /* drop the CR of a CRLF pair: rebuild minus one byte */
                const char *bytes = str_cstr(line);
                char *copy = arena_strndup(str_arena(line), bytes, n - 1);
                str_clear(line);
                str_append(line, copy);
            }
            break;
        }
        str_push(line, (char)c);
        c = fgetc(f);
    }
    return 1;
}

/* PORT: `Copy(Line, 1, 8) = '#include'` (drf.pas:172/187) - the line's
   first eight characters, exactly, case-sensitively. */
static int is_include_line(const Str *line)
{
    return str_len(line) >= 8 &&
           memcmp(str_bytes(line), "#include", 8) == 0;
}

/* PORT: FPC sysutils Trim - strips bytes <= ' ' (0x20) at both ends. */
static const char *trim_dup(Arena *a, const char *s, size_t len)
{
    size_t start = 0, end = len;
    while (start < end && (unsigned char)s[start] <= 0x20) start++;
    while (end > start && (unsigned char)s[end - 1] <= 0x20) end--;
    return arena_strndup(a, s + start, end - start);
}

/* PORT: drf.pas:174-176 - IncludeFileName := Copy(Line,10,MaxInt)
   (character 9, whatever it is, is discarded as the separator), then
   truncated at the first ';' (comment), then trimmed. */
static const char *include_filename(Arena *a, const Str *line)
{
    const char *bytes = (const char *)str_bytes(line);
    size_t len = str_len(line);
    const char *tail;
    size_t tail_len;
    const char *semi;

    if (len <= 9) {
        tail = "";
        tail_len = 0;
    } else {
        tail = bytes + 9;
        tail_len = len - 9;
    }
    semi = memchr(tail, ';', tail_len);
    if (semi != NULL) tail_len = (size_t)(semi - tail);
    return trim_dup(a, tail, tail_len);
}

static int file_exists(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* PORT: WriteLn(TempFile, Line) (drf.pas:188/199). The reference
   platform's FPC WriteLn emits CRLF; pinned here unconditionally,
   matching the phase's JSON line-ending PORT NOTE (the temp file is
   consumed by lexlib, which collapses either ending, so this is
   observable only to anything reading the temp file's raw bytes). */
static void write_line(FILE *f, const Str *line)
{
    fwrite(str_bytes(line), 1, str_len(line), f);
    fputs("\r\n", f);
}

int preparse(Arena *a, Diag *d, const char *input_filename,
              const char *temp_filename)
{
    FILE *input, *temp;
    Str *line = str_new(a);
    long current_line, temp_line;

    /* PORT NOTE: the Pascal never resets IncludeList (single-shot
       process); reset here so each compile starts a fresh map. */
    include_reset();

    input = fopen(input_filename, "rb");
    if (input == NULL) {
        /* Guard only: drf.pas checked FileExists(InputFileName) at
           the CLI stage (drf.pas:338) before ever reaching Preparse,
           so Reset cannot fail there; a direct caller of this port
           (tests) could still race or typo. */
        diag_fatal(d, "cannot open \"%s\"", input_filename);
        return 0;
    }
    temp = fopen(temp_filename, "wb");
    if (temp == NULL) {
        /* Guard only: FPC Rewrite would raise a runtime error here. */
        fclose(input);
        diag_fatal(d, "cannot create \"%s\"", temp_filename);
        return 0;
    }

    current_line = 0;
    temp_line = 0;
    while (read_line(input, line)) {
        current_line++;
        if (is_include_line(line)) {
            const char *inc_name = include_filename(a, line);
            FILE *inc;
            long preserve_current_line;

            if (!file_exists(inc_name)) {
                /* PORT: PreparseError (drf.pas:56-60,177): shape
                   `<line>:0: <msg>.`, Halt(2). */
                diag_preparse_error(d, current_line,
                                     "Include file \"%s\" not found",
                                     inc_name);
                fclose(input);
                fclose(temp);
                return 0;
            }
            diag_verbose(d, "Including %s...", inc_name);
            inc = fopen(inc_name, "rb");
            if (inc == NULL) {
                /* Guard only: exists-check just passed; FPC's Reset
                   would raise a runtime error on this race. */
                diag_fatal(d, "cannot open \"%s\"", inc_name);
                fclose(input);
                fclose(temp);
                return 0;
            }
            preserve_current_line = current_line;
            current_line = 0;
            while (read_line(inc, line)) {
                current_line++;
                if (is_include_line(line)) {
                    /* PORT: drf.pas:187 - reported with the
                       include-LOCAL line number. */
                    diag_preparse_error(d, current_line,
                                         "Nested includes are not allowed");
                    fclose(inc);
                    fclose(input);
                    fclose(temp);
                    return 0;
                }
                write_line(temp, line);
                temp_line++;
                include_add_line(a, temp_line, inc_name, current_line);
            }
            fclose(inc);
            current_line = preserve_current_line;
        } else {
            write_line(temp, line);
            temp_line++;
            include_add_line(a, temp_line, input_filename, current_line);
        }
    }
    fclose(input);
    fclose(temp);
    return 1;
}
