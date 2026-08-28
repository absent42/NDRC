/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_diag.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "diag.h"

#include <stdio.h>

/* Diagnostics are written to a stream the test redirects to a temp file,
   so formatting is asserted on real output rather than on an internal
   buffer that might diverge from what a user sees. Shapes are asserted
   byte-exactly, including the trailing period DRC appends itself, and
   mirror the verified live sample:
       2:13:g.DSF: "CAF" already defined. */
static FILE *scratch_open(void)
{
    return tmpfile();
}

static void scratch_read(FILE *f, char *buf, size_t n)
{
    size_t got;
    rewind(f);
    got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
}

TEST(diag_starts_clean)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(diag_warn_count(d), 0);
    CHECK(diag_last(d) == NULL);
    CHECK(diag_last_error(d) == NULL);
    CHECK_INT(diag_exit_code(d), 0);
    arena_free(a);
}

TEST(diag_syntax_error_shape_and_class)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_set_source(d, "g.DSF");
    diag_syntax_error(d, 2, 13, "\"%s\" already defined", "CAF");
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "2:13:g.DSF: \"CAF\" already defined.\n");
    CHECK_INT(diag_error_count(d), 1);
    CHECK_STR(diag_last(d), "\"CAF\" already defined");
    CHECK_STR(diag_last_error(d), "\"CAF\" already defined");
    CHECK_INT(diag_exit_code(d), 1);
    fclose(f);
    arena_free(a);
}

TEST(diag_warn_shape_and_no_exit_class)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_set_source(d, "game.dsf");
    diag_warn(d, 3, 1, "object %d is never created", 12);
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "Warning: 3:1:game.dsf: object 12 is never created.\n");
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(diag_warn_count(d), 1);
    CHECK_STR(diag_last(d), "object 12 is never created");
    CHECK(diag_last_error(d) == NULL);
    CHECK_INT(diag_exit_code(d), 0);
    fclose(f);
    arena_free(a);
}

TEST(diag_fatal_shape_and_class)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_fatal(d, "cannot open %s", "missing.ddb");
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "Error: cannot open missing.ddb.\n");
    CHECK_INT(diag_error_count(d), 1);
    CHECK_STR(diag_last(d), "cannot open missing.ddb");
    CHECK_STR(diag_last_error(d), "cannot open missing.ddb");
    CHECK_INT(diag_exit_code(d), 2);
    fclose(f);
    arena_free(a);
}

TEST(diag_exit_code_first_error_wins_syntax_then_fatal)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    diag_set_stream(d, f);
    diag_set_source(d, "g.dsf");
    diag_syntax_error(d, 1, 1, "first");
    CHECK_INT(diag_exit_code(d), 1);
    diag_fatal(d, "second");
    CHECK_INT(diag_error_count(d), 2);
    CHECK_INT(diag_exit_code(d), 1);
    fclose(f);
    arena_free(a);
}

TEST(diag_exit_code_stable_across_interleaved_warning)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    diag_set_stream(d, f);
    diag_set_source(d, "g.dsf");
    diag_syntax_error(d, 1, 1, "first");
    diag_warn(d, 2, 1, "an interleaved warning");
    CHECK_INT(diag_exit_code(d), 1);
    CHECK_INT(diag_error_count(d), 1);
    fclose(f);
    arena_free(a);
}

TEST(diag_exit_code_first_error_wins_fatal_then_syntax)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    diag_set_stream(d, f);
    diag_set_source(d, "g.dsf");
    diag_fatal(d, "first");
    CHECK_INT(diag_exit_code(d), 2);
    diag_syntax_error(d, 1, 1, "second");
    CHECK_INT(diag_error_count(d), 2);
    CHECK_INT(diag_exit_code(d), 2);
    fclose(f);
    arena_free(a);
}

TEST(diag_last_vs_last_error_diverge_after_warning)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    diag_set_stream(d, f);
    diag_set_source(d, "g.dsf");
    diag_syntax_error(d, 1, 1, "an error");
    CHECK_STR(diag_last(d), "an error");
    CHECK_STR(diag_last_error(d), "an error");
    diag_warn(d, 2, 1, "a warning");
    /* diag_last tracks the most recent diagnostic of EITHER severity, so
       it moves to the warning - but diag_last_error stays on the error,
       which is the whole point of having both accessors. */
    CHECK_STR(diag_last(d), "a warning");
    CHECK_STR(diag_last_error(d), "an error");
    fclose(f);
    arena_free(a);
}

TEST(diag_note_does_not_count_as_error)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_note(d, "loaded %d messages", 40);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(diag_warn_count(d), 0);
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "loaded 40 messages\n");
    fclose(f);
    arena_free(a);
}

TEST(diag_verbose_is_silent_unless_enabled)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_verbose(d, "this must not appear");
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "");
    fclose(f);
    arena_free(a);
}

TEST(diag_verbose_prints_when_enabled)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_set_verbose(d, 1);
    diag_verbose(d, "symbol table holds %d entries", 128);
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "symbol table holds 128 entries\n");
    fclose(f);
    arena_free(a);
}

TEST(diag_counts_accumulate)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    diag_set_stream(d, f);
    diag_set_source(d, "g.dsf");
    diag_syntax_error(d, 1, 1, "first");
    diag_syntax_error(d, 2, 1, "second");
    diag_warn(d, 3, 1, "third");
    CHECK_INT(diag_error_count(d), 2);
    CHECK_INT(diag_warn_count(d), 1);
    /* diag_last tracks the most recent diagnostic of EITHER severity, as
       diag.h documents - so the warning, not the last error. */
    CHECK_STR(diag_last(d), "third");
    CHECK_STR(diag_last_error(d), "second");
    CHECK_INT(diag_exit_code(d), 1);
    fclose(f);
    arena_free(a);
}

TEST(diag_param_error_shape_and_class)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_param_error(d, "Invalid option: %s", "-bogus");
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "Invalid option: -bogus.\n");
    CHECK_INT(diag_error_count(d), 1);
    CHECK_STR(diag_last(d), "Invalid option: -bogus");
    CHECK_STR(diag_last_error(d), "Invalid option: -bogus");
    CHECK_INT(diag_exit_code(d), 2);
    fclose(f);
    arena_free(a);
}

TEST(diag_param_error_no_source_needed)
{
    /* Unlike diag_syntax_error/diag_warn, diag_param_error carries no
       filename and must not require diag_set_source. */
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    diag_set_stream(d, f);
    diag_param_error(d, "Input file not found: \"%s\"", "nosuchfile.dsf");
    scratch_read(f, buf, sizeof(buf));
    CHECK_STR(buf, "Input file not found: \"nosuchfile.dsf\".\n");
    CHECK_INT(diag_exit_code(d), 2);
    fclose(f);
    arena_free(a);
}

int main(void)
{
    RUN(diag_starts_clean);
    RUN(diag_syntax_error_shape_and_class);
    RUN(diag_warn_shape_and_no_exit_class);
    RUN(diag_fatal_shape_and_class);
    RUN(diag_exit_code_first_error_wins_syntax_then_fatal);
    RUN(diag_exit_code_stable_across_interleaved_warning);
    RUN(diag_exit_code_first_error_wins_fatal_then_syntax);
    RUN(diag_last_vs_last_error_diverge_after_warning);
    RUN(diag_note_does_not_count_as_error);
    RUN(diag_verbose_is_silent_unless_enabled);
    RUN(diag_verbose_prints_when_enabled);
    RUN(diag_counts_accumulate);
    RUN(diag_param_error_shape_and_class);
    RUN(diag_param_error_no_source_needed);
    return test_summary("diag");
}
