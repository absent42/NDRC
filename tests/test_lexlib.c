/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_lexlib.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "../src/front/lexlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/* Scratch files go through TMPDIR/TEMP/TMP (the test_finish.c
   approach), not the CWD, so a crashed run leaves cruft in the OS
   temp directory rather than as untracked files in the repo root. */
static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) return;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* Opens `path` and asserts that get_char() yields exactly the
   `n`-byte `expect` sequence, then keeps returning 0 (true EOF) on
   further calls - matching ULexLib.pas: get_char returns #0 forever
   once the file and buffer are both exhausted. */
static void expect_sequence(const char *path, const char *expect)
{
    size_t i, n = strlen(expect);

    CHECK(lexlib_open(path) != 0);
    for (i = 0; i < n; i++)
        CHECK_INT(get_char(), (unsigned char)expect[i]);
    CHECK_INT(get_char(), 0);
    CHECK_INT(get_char(), 0);
    lexlib_close();
}

/* (a) A 2-line CRLF file yields the exact char sequence with the
   synthetic trailing '\n' ULexLib.pas's get_char appends per line -
   the CRLF pair collapses to that one synthetic '\n', matching FPC's
   readln (ULexLib.pas:189). */
TEST(two_line_crlf_file_yields_sequence_with_synthetic_newlines)
{
    char path[512];
    scratch_path(path, sizeof path, "test_lexlib_crlf.txt");
    write_file(path, "Hello\r\nWorld\r\n");
    expect_sequence(path, "Hello\nWorld\n");
    remove(path);
}

/* (b) The same two lines, LF-only with no trailing newline, yield
   an identical sequence: FPC's readln collapses CRLF and bare LF to
   the same synthetic '\n' even at EOF (ULexLib.pas:187's eof(yyinput)
   guard fires only when nothing is left to read). */
TEST(same_bytes_lf_only_no_trailing_newline_yield_identical_sequence)
{
    char path[512];
    scratch_path(path, sizeof path, "test_lexlib_lf.txt");
    write_file(path, "Hello\nWorld");
    expect_sequence(path, "Hello\nWorld\n");
    remove(path);
}

/* (c) yylineno/yycolno track the scan position exactly per
   ULexLib.pas:183-208: yycolno is set to 1 when a line loads, then
   incremented by every get_char (AFTER the character is fetched), so
   immediately after get_char returns the Nth character (1-based) of
   the line, yycolno == N + 1. yylineno starts at 0 and becomes 1 on
   the (lazy) first line load. Neither is touched on the terminal
   true-EOF path - hand-verified against ULexLib.pas:200-207. */
TEST(yylineno_and_yycolno_track_the_scan_position)
{
    char path[512];
    scratch_path(path, sizeof path, "test_lexlib_colno.txt");
    write_file(path, "AB\n");

    CHECK(lexlib_open(path) != 0);
    CHECK_INT(yylineno, 0);   /* no line loaded yet - lazy load */

    CHECK_INT(get_char(), 'A');
    CHECK_INT(yylineno, 1);
    CHECK_INT(yycolno, 2);    /* col of 'A' (N=1) plus 1 */

    CHECK_INT(get_char(), 'B');
    CHECK_INT(yycolno, 3);    /* col of 'B' (N=2) plus 1 */

    CHECK_INT(get_char(), '\n');  /* synthetic trailing newline */
    CHECK_INT(yycolno, 4);

    CHECK_INT(get_char(), 0);     /* true EOF */
    CHECK_INT(yylineno, 1);       /* unchanged on the EOF path */
    CHECK_INT(yycolno, 4);        /* unchanged on the EOF path */

    lexlib_close();
    remove(path);
}

/* (d) unget_char pushback round-trips: a pushed-back character is
   returned again by the next get_char, and yycolno is restored
   symmetrically (unget_char decrements exactly what get_char
   incremented, ULexLib.pas:200-216). The mechanism does not care
   what value is pushed - it is a plain stack - so an arbitrary
   pushed value round-trips too, exactly as yyless's char-by-char
   unget_char calls rely on (ULexLib.pas:278-289). */
TEST(unget_char_pushback_round_trips)
{
    char path[512];
    scratch_path(path, sizeof path, "test_lexlib_pushback.txt");
    write_file(path, "AB\n");

    CHECK(lexlib_open(path) != 0);

    CHECK_INT(get_char(), 'A');
    CHECK_INT(yycolno, 2);

    CHECK_INT(get_char(), 'B');
    CHECK_INT(yycolno, 3);

    unget_char('B');
    CHECK_INT(yycolno, 2);        /* symmetric restore */
    CHECK_INT(get_char(), 'B');   /* same value read again */
    CHECK_INT(yycolno, 3);

    /* Arbitrary pushed value, not tied to what was last read. */
    unget_char((unsigned char)'Z');
    CHECK_INT(get_char(), 'Z');

    lexlib_close();
    remove(path);
}

/* (e) The 8192/8191 boundary, pinned from both sides. ULexLib.pas's
   line-copy loop (ULexLib.pas:193-197) has no bounds check - overrun
   is memory corruption - so this port's FATAL guard (exit(1) via
   lexlib_fatal) must be exercised in a CHILD PROCESS here, since it
   would tear down this test binary if triggered in-process. */

static const char *g_self;   /* argv[0], captured by main() below */

static void write_single_line_of(const char *path, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    CHECK(f != NULL);
    if (f == NULL) return;
    for (i = 0; i < len; i++) fputc('x', f);
    fputc('\n', f);
    fclose(f);
}

/* Normalises system()'s return value to the child's real exit() code
   on every platform this suite runs on (POSIX CI needs WEXITSTATUS;
   Windows/MinGW's system() already returns the raw code). -1 if the
   child did not exit normally at all. */
static int child_exit_code(int status)
{
#ifdef _WIN32
    return status;
#else
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

static int run_guard_child(const char *path)
{
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "%s --lexlib-guard-child \"%s\"", g_self, path);
    return child_exit_code(system(cmd));
}

TEST(line_of_8192_chars_trips_the_length_guard)
{
    char path[512];
    scratch_path(path, sizeof path, "test_lexlib_8192.txt");
    write_single_line_of(path, 8192);

    CHECK_INT(run_guard_child(path), 1);   /* lexlib_fatal's exit(1) */

    remove(path);
}

TEST(line_of_8191_chars_fills_buffer_exactly_and_lexes_fine)
{
    char path[512];
    scratch_path(path, sizeof path, "test_lexlib_8191.txt");
    write_single_line_of(path, 8191);

    /* run_guard_child's child returns 0 only if the guard did NOT
       fire AND it consumed exactly the 8192 characters (8191 'x'
       bytes plus the synthetic trailing '\n') the line should
       produce - see guard_child_main below. */
    CHECK_INT(run_guard_child(path), 0);

    remove(path);
}

/* Runs in the child process spawned by run_guard_child above: drives
   get_char() to completion on `path` and reports the outcome via its
   own exit code (never returned to a caller - always via exit()/
   process termination, either here or inside lexlib_fatal):
     0 - finished cleanly, exactly 8192 non-NUL characters consumed
     1 - never reached directly; this is lexlib_fatal's own exit(1)
     3 - lexlib_open failed
     4 - runaway safety valve (something is very wrong)
     5 - finished cleanly but the character count was wrong */
static int guard_child_main(const char *path)
{
    long consumed = 0;
    unsigned char c;

    if (!lexlib_open(path)) return 3;
    for (;;) {
        c = get_char();
        if (c == 0) break;
        consumed++;
        if (consumed > 100000) return 4;
    }
    lexlib_close();
    return (consumed == 8192) ? 0 : 5;
}

int main(int argc, char **argv)
{
    g_self = argv[0];

    if (argc >= 3 && strcmp(argv[1], "--lexlib-guard-child") == 0)
        return guard_child_main(argv[2]);

    RUN(two_line_crlf_file_yields_sequence_with_synthetic_newlines);
    RUN(same_bytes_lf_only_no_trailing_newline_yield_identical_sequence);
    RUN(yylineno_and_yycolno_track_the_scan_position);
    RUN(unget_char_pushback_round_trips);
    RUN(line_of_8192_chars_trips_the_length_guard);
    RUN(line_of_8191_chars_fills_buffer_exactly_and_lexes_fine);
    return test_summary("lexlib");
}
