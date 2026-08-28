/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/diag.c - Copyright (C) 2026 Dan Gibson. */
#include "diag.h"
#include "str.h"

#include <stdarg.h>
#include <stdlib.h>

struct Diag {
    Arena *arena;
    const char *source;
    FILE *out;
    int verbose;
    int errors;
    int warnings;
    const char *last;
    const char *last_error;
    int first_error_class;
};

Diag *diag_new(Arena *a)
{
    Diag *d = arena_alloc(a, sizeof(*d));
    d->arena = a;
    d->source = NULL;
    d->out = stderr;
    d->verbose = 0;
    d->errors = 0;
    d->warnings = 0;
    d->last = NULL;
    d->last_error = NULL;
    d->first_error_class = 0;
    return d;
}

void diag_set_source(Diag *d, const char *filename)
{
    d->source = filename ? arena_strdup(d->arena, filename) : NULL;
}

void diag_set_verbose(Diag *d, int on)
{
    d->verbose = on;
}

void diag_set_stream(Diag *d, FILE *out)
{
    d->out = out;
}

/* Formats fmt/ap into an arena-owned, NUL-terminated string. This is
   the message TEXT only - no position, severity or trailing period. */
static char *diag_format_body(Diag *d, const char *fmt, va_list ap)
{
    char *body;
    va_list copy;

    va_copy(copy, ap);
    {
        int n = vsnprintf(NULL, 0, fmt, copy);
        va_end(copy);
        body = arena_alloc(d->arena, (size_t)(n < 0 ? 0 : n) + 1u);
        vsnprintf(body, (size_t)(n < 0 ? 0 : n) + 1u, fmt, ap);
    }
    return body;
}

void diag_syntax_error(Diag *d, int line, int col, const char *fmt, ...)
{
    va_list ap;
    char *body;
    Str *msg;

    if (d->source == NULL) {
        fprintf(stderr,
                "ndrc: internal error, %s called before diag_set_source\n",
                "diag_syntax_error");
        abort();
    }

    msg = str_new(d->arena);
    va_start(ap, fmt);
    body = diag_format_body(d, fmt, ap);
    va_end(ap);

    str_appendf(msg, "%d:%d:%s: %s.\n", line, col, d->source, body);
    fputs(str_cstr(msg), d->out);
    fflush(d->out);

    d->last = body;
    d->last_error = body;
    d->errors++;
    if (d->first_error_class == 0) d->first_error_class = 1;
}

void diag_warn(Diag *d, int line, int col, const char *fmt, ...)
{
    va_list ap;
    char *body;
    Str *msg;

    if (d->source == NULL) {
        fprintf(stderr,
                "ndrc: internal error, %s called before diag_set_source\n",
                "diag_warn");
        abort();
    }

    msg = str_new(d->arena);
    va_start(ap, fmt);
    body = diag_format_body(d, fmt, ap);
    va_end(ap);

    str_appendf(msg, "Warning: %d:%d:%s: %s.\n", line, col, d->source, body);
    fputs(str_cstr(msg), d->out);
    fflush(d->out);

    d->last = body;
    d->warnings++;
}

void diag_fatal(Diag *d, const char *fmt, ...)
{
    va_list ap;
    char *body;
    Str *msg = str_new(d->arena);

    va_start(ap, fmt);
    body = diag_format_body(d, fmt, ap);
    va_end(ap);

    str_appendf(msg, "Error: %s.\n", body);
    fputs(str_cstr(msg), d->out);
    fflush(d->out);

    d->last = body;
    d->last_error = body;
    d->errors++;
    if (d->first_error_class == 0) d->first_error_class = 2;
}

void diag_preparse_error(Diag *d, long line, const char *fmt, ...)
{
    va_list ap;
    char *body;
    Str *msg = str_new(d->arena);

    va_start(ap, fmt);
    body = diag_format_body(d, fmt, ap);
    va_end(ap);

    /* PORT: PreparseError (drf.pas:56-60): WriteLn(Currentline,':0: ',
       Msg,'.') then Halt(2) - line and a fixed 0 column, NO filename
       (live-verified against drf.exe 2026-08-27: `1:0: Include file
       "missing_file.inc" not found.`, exit 2). */
    str_appendf(msg, "%ld:0: %s.\n", line, body);
    fputs(str_cstr(msg), d->out);
    fflush(d->out);

    d->last = body;
    d->last_error = body;
    d->errors++;
    if (d->first_error_class == 0) d->first_error_class = 2;
}

void diag_param_error(Diag *d, const char *fmt, ...)
{
    va_list ap;
    char *body;
    Str *msg = str_new(d->arena);

    va_start(ap, fmt);
    body = diag_format_body(d, fmt, ap);
    va_end(ap);

    /* PORT: ParamError (drf.pas:50-54): WriteLn(Msg, '.') then Halt(2) -
       bare message, NO "Error: " prefix (unlike diag_fatal, DRB's
       shape), no position, no filename (live-verified against drf.exe
       2026-08-27: `Invalid option: -bogus.`, `Input file not found:
       "nosuchfile.dsf".`, both exit 2). */
    str_appendf(msg, "%s.\n", body);
    fputs(str_cstr(msg), d->out);
    fflush(d->out);

    d->last = body;
    d->last_error = body;
    d->errors++;
    if (d->first_error_class == 0) d->first_error_class = 2;
}

static void diag_plain(Diag *d, const char *fmt, va_list ap)
{
    vfprintf(d->out, fmt, ap);
    fputc('\n', d->out);
    fflush(d->out);
}

void diag_note(Diag *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_plain(d, fmt, ap);
    va_end(ap);
}

void diag_note_raw(Diag *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(d->out, fmt, ap);
    fflush(d->out);
    va_end(ap);
}

void diag_verbose(Diag *d, const char *fmt, ...)
{
    va_list ap;
    if (!d->verbose) return;
    va_start(ap, fmt);
    diag_plain(d, fmt, ap);
    va_end(ap);
}

int diag_error_count(const Diag *d)
{
    return d->errors;
}

int diag_warn_count(const Diag *d)
{
    return d->warnings;
}

const char *diag_last(const Diag *d)
{
    return d->last;
}

const char *diag_last_error(const Diag *d)
{
    return d->last_error;
}

int diag_exit_code(const Diag *d)
{
    return d->first_error_class;
}
