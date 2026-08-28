/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_lex.c - Copyright (C) 2026 Dan Gibson.

   Each vector below cites its DSF.l rule number (lexer.pas's yyaction
   case, same numbering) or its docs/dev/drc-analysis.md catalogue entry.
   DSF inputs go through temp files (the test_finish.c/test_lexlib.c
   scratch_path approach - TMPDIR/TEMP/TMP, "." only as a last
   resort), since lex_tokenize's only surface is a path. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "../src/front/lex_tokens.h"
#include "../src/front/tokenlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scratch_path(char *buf, size_t bufsz, const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, filename);
}

static void write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) return;
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Bundles a fresh Arena/Diag (diag's stream redirected to a scratch
   temp file, so error text can be asserted byte-exactly) with
   lex_tokenize's result, and the path passed in - every diagnostic
   lex_tokenize can raise embeds this same path as its "source"
   (lex_tokenize calls diag_set_source(d, path) itself). */
typedef struct {
    Arena *a;
    Diag *d;
    FILE *diagf;
    char path[512];
    Token *head;
} LexRun;

static LexRun run_lex(const char *filename, const void *data, size_t len)
{
    LexRun r;
    scratch_path(r.path, sizeof r.path, filename);
    write_bytes(r.path, data, len);
    r.a = arena_new(0);
    r.d = diag_new(r.a);
    r.diagf = tmpfile();
    diag_set_stream(r.d, r.diagf);
    r.head = lex_tokenize(r.a, r.d, r.path);
    return r;
}

static void run_lex_cleanup(LexRun *r)
{
    fclose(r->diagf);
    remove(r->path);
    arena_free(r->a);
}

static void diag_read(LexRun *r, char *buf, size_t n)
{
    size_t got;
    rewind(r->diagf);
    got = fread(buf, 1, n - 1, r->diagf);
    buf[got] = '\0';
}

/* Section markers (DSF.l:15-24, rules 1-10). */
TEST(section_markers_lex_in_order_with_verbatim_text)
{
    static const char *src =
        "/CTL\n/STX\n/CON\n/MTX\n/LTX\n/OTX\n/OBJ\n/VOC\n/PRO\n/END\n";
    static const int want_ids[10] = {
        T_SECTION_CTL, T_SECTION_STX, T_SECTION_CON, T_SECTION_MTX,
        T_SECTION_LTX, T_SECTION_OTX, T_SECTION_OBJ, T_SECTION_VOC,
        T_SECTION_PRO, T_SECTION_END
    };
    static const char *want_text[10] = {
        "/CTL", "/STX", "/CON", "/MTX", "/LTX", "/OTX", "/OBJ", "/VOC",
        "/PRO", "/END"
    };
    LexRun r = run_lex("test_lex_sections.dsf", src, strlen(src));
    Token *t = r.head;
    int i;

    CHECK(r.head != NULL);
    for (i = 0; i < 10 && t != NULL; i++, t = t->next) {
        CHECK_INT(t->id, want_ids[i]);
        CHECK_STR(t->text, want_text[i]);
        CHECK_INT(t->value, TOKEN_NO_VALUE);
    }
    CHECK_INT(i, 10);
    CHECK(t == NULL);   /* nothing left over */

    run_lex_cleanup(&r);
}

/* Directives (DSF.l:25-43, rules 11-29), one of each. */
TEST(all_directive_keywords_lex_to_their_token_ids)
{
    static const char *src =
        "#define\n#ifdef\n#ifndef\n#endif\n#else\n#echo\n#userptr\n"
        "#int\n#sfx\n#hex\n#debug\n#db\n#defb\n#dw\n#defw\n#extern\n"
        "#incbin\n#classic\n";
    static const int want_ids[18] = {
        T_DEFINE, T_IFDEF, T_IFNDEF, T_ENDIF, T_ELSE, T_ECHO, T_USERPTR,
        T_INT, T_SFX, T_HEX, T_DEBUG, T_DB, T_DB, T_DW, T_DW, T_EXTERN,
        T_INCBIN, T_CLASSIC
    };
    LexRun r = run_lex("test_lex_directives.dsf", src, strlen(src));
    Token *t = r.head;
    int i;

    CHECK(r.head != NULL);
    for (i = 0; i < 18 && t != NULL; i++, t = t->next)
        CHECK_INT(t->id, want_ids[i]);
    CHECK_INT(i, 18);
    CHECK(t == NULL);

    run_lex_cleanup(&r);
}

/* 19.9 / 15.1 quirk (a): "#if" needs no trailing space - the apparent
   space in DSF.l:27 is the pattern/action separator, not part of the
   pattern. "#ifx" lexes as T_IFDEF ("#if") immediately followed by
   T_IDENTIFIER ("x"), consuming no delimiter at all. */
TEST(bare_if_directive_self_terminates_before_an_identifier)
{
    LexRun r = run_lex("test_lex_bareif.dsf", "#ifx\n", 5);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_IFDEF);
        CHECK_STR(r.head->text, "#if");
        CHECK(r.head->next != NULL);
        if (r.head->next != NULL) {
            CHECK_INT(r.head->next->id, T_IDENTIFIER);
            CHECK_STR(r.head->next->text, "x");
            CHECK(r.head->next->next == NULL);
        }
    }

    run_lex_cleanup(&r);
}

/* 19.7 / 15.1 quirk (b): a double-quoted string is greedy to the LAST
   quote on the line - "a" "b" is ONE T_STRING token, embedded quotes
   and space included verbatim (DSF.l:44, rule 30). */
TEST(double_quoted_string_is_greedy_to_the_last_quote_on_the_line)
{
    LexRun r = run_lex("test_lex_dqgreedy.dsf", "\"a\" \"b\"\n", 8);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_STRING);
        CHECK_STR(r.head->text, "\"a\" \"b\"");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* DSF.l:46, rule 32: a lone "_" is T_UNDERSCORE (longest match against
   the identifier rule, which also accepts a bare "_" as a 1-char
   identifier, ties broken by DSF.l's declaration order - "_" is
   declared first). */
TEST(lone_underscore_is_t_underscore)
{
    LexRun r = run_lex("test_lex_underscore.dsf", "_\n", 2);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_UNDERSCORE);
        CHECK_STR(r.head->text, "_");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
        CHECK_INT(r.head->line, 1);
        CHECK_INT(r.head->col, 1);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* DSF.l:47, rule 33: "*" is also T_UNDERSCORE - a second spelling for
   the same token id (state 7 is dead: no further transition class, so
   unlike "_" this one never has to look ahead at all). */
TEST(star_is_also_t_underscore)
{
    LexRun r = run_lex("test_lex_star.dsf", "*\n", 2);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_UNDERSCORE);
        CHECK_STR(r.head->text, "*");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
        CHECK_INT(r.head->line, 1);
        CHECK_INT(r.head->col, 1);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* DSF.l:48, rule 34: ">" is T_PROCESS_ENTRY_SIGN - DSF's process-entry
   header marker, its single most common token in real source. */
TEST(process_entry_sign)
{
    LexRun r = run_lex("test_lex_procentry.dsf", ">\n", 2);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_PROCESS_ENTRY_SIGN);
        CHECK_STR(r.head->text, ">");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
        CHECK_INT(r.head->line, 1);
        CHECK_INT(r.head->col, 1);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* DSF.l:55, rule 41: "@" is T_INDIRECT, the indirection marker. */
TEST(indirection_marker)
{
    LexRun r = run_lex("test_lex_indirect.dsf", "@\n", 2);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_INDIRECT);
        CHECK_STR(r.head->text, "@");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
        CHECK_INT(r.head->line, 1);
        CHECK_INT(r.head->col, 1);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* 19.6 / 15.1 quirk (c): 'abc' (a single closing quote) never reaches
   rule 31's accepting state - the deepest reachable accept is the
   rule-43 catch-all, so it is a hard lexer error at the OPENING
   quote. */
TEST(single_quoted_string_with_one_closing_quote_is_a_lexer_error)
{
    LexRun r = run_lex("test_lex_sqerror.dsf", "'abc'\n", 6);

    CHECK(r.head == NULL);
    CHECK_INT(diag_error_count(r.d), 1);
    CHECK_INT(diag_exit_code(r.d), 1);   /* diag_syntax_error's class */

    run_lex_cleanup(&r);
}

/* 19.6 / 15.1 quirk (c): 'abc'' (a DOUBLED closing quote) is the
   minimum accepted form for rule 31, and DOES run UTF8Encode(yytext)
   - here an identity transform, since every byte is plain ASCII.
   lex_tokenize stores the UTF8-encoded FULL match (quotes included,
   DSF.l:45); stripping one quote off each end to recover the leaked-
   quote content "abc'" is the PARSER's job (USintactic.pas's
   Copy(CurrentText,2,Length-2)). */
TEST(single_quoted_string_with_doubled_closing_quote_is_one_string_token)
{
    LexRun r = run_lex("test_lex_sqdoubled.dsf", "'abc''\n", 7);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_STRING);
        CHECK_STR(r.head->text, "'abc''");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* Empirically pinned against D:/DRC/src/drf.exe (branch nextdaad,
   2026-08-27 - see lex.c's utf8_encode_cp1252 header note): UTF8Encode
   treats the source byte as CP1252, not
   plain ISO-8859-1. Byte 0x80 (undefined in ISO-8859-1's usual
   identity reading, but CP1252's EURO SIGN) must encode as the
   3-byte UTF-8 sequence for U+20AC (E2 82 AC), and byte 0xE1
   (a-acute, identical in both encodings) must encode as the ordinary
   2-byte UTF-8 sequence for U+00E1 (C3 A1). */
TEST(single_quoted_string_utf8encode_matches_cp1252_not_plain_latin1)
{
    unsigned char src1[] = { '\'', 0x80, '\'', '\'', '\n' };
    unsigned char src2[] = { '\'', 0xE1, '\'', '\'', '\n' };
    LexRun r1 = run_lex("test_lex_cp1252_80.dsf", src1, sizeof src1);
    LexRun r2 = run_lex("test_lex_cp1252_e1.dsf", src2, sizeof src2);

    CHECK(r1.head != NULL);
    if (r1.head != NULL)
        CHECK_MEM(r1.head->text, "'\xE2\x82\xAC''", 6);   /* ' + E2 82 AC + '' */
    CHECK(r2.head != NULL);
    if (r2.head != NULL)
        CHECK_MEM(r2.head->text, "'\xC3\xA1''", 5);        /* ' + C3 A1 + '' */

    run_lex_cleanup(&r1);
    run_lex_cleanup(&r2);
}

/* 19.8 / 15.1 quirk (d): a class typo in DSF.l puts a literal comma in
   the identifier character class - a bare "," lexes as T_IDENTIFIER,
   and commas embed silently inside otherwise-normal names. */
TEST(comma_is_an_identifier_character)
{
    LexRun r1 = run_lex("test_lex_comma1.dsf", ",\n", 2);
    LexRun r2 = run_lex("test_lex_comma2.dsf", "a,b\n", 4);

    CHECK(r1.head != NULL);
    if (r1.head != NULL) {
        CHECK_INT(r1.head->id, T_IDENTIFIER);
        CHECK_STR(r1.head->text, ",");
    }
    CHECK(r2.head != NULL);
    if (r2.head != NULL) {
        CHECK_INT(r2.head->id, T_IDENTIFIER);
        CHECK_STR(r2.head->text, "a,b");
        CHECK(r2.head->next == NULL);
    }

    run_lex_cleanup(&r1);
    run_lex_cleanup(&r2);
}

/* 19.15 / 15.1 quirk (e): "123abc" is T_IDENTIFIER by longest match (a
   digit run also accepts as a number, but continuing into letters
   wins); a lone "-" or "-abc" is a lexer error - the "-" state
   transitions only on digits. */
TEST(digit_letter_run_is_identifier_but_dash_letter_is_an_error)
{
    LexRun r1 = run_lex("test_lex_digitid.dsf", "123abc\n", 7);
    LexRun r2 = run_lex("test_lex_dashonly.dsf", "-\n", 2);
    LexRun r3 = run_lex("test_lex_dashabc.dsf", "-abc\n", 5);

    CHECK(r1.head != NULL);
    if (r1.head != NULL) {
        CHECK_INT(r1.head->id, T_IDENTIFIER);
        CHECK_STR(r1.head->text, "123abc");
        CHECK(r1.head->next == NULL);
    }
    CHECK(r2.head == NULL);
    CHECK_INT(diag_error_count(r2.d), 1);
    CHECK(r3.head == NULL);
    CHECK_INT(diag_error_count(r3.d), 1);

    run_lex_cleanup(&r1);
    run_lex_cleanup(&r2);
    run_lex_cleanup(&r3);
}

/* Rule 38: -?[0-9]+, sign included in both text and the StrToInt'd
   value. */
TEST(negative_number_literal)
{
    LexRun r = run_lex("test_lex_negnum.dsf", "-42\n", 4);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_NUMBER);
        CHECK_STR(r.head->text, "-42");
        CHECK_INT(r.head->value, -42);
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* 19.13: the literal 2147483647 IS representable (it is exactly
   int32's max, i.e. Pascal's MaxLongInt) - lex_tokenize computes it
   correctly; that it then collides with AddToken's own "no value"
   sentinel and gets rejected as an unknown symbol is the PARSER's
   concern (section 15.3), out of this task's scope. -2147483648
   (int32's min) is the boundary companion, also representable. */
TEST(int32_boundary_literals_both_fit)
{
    LexRun r1 = run_lex("test_lex_intmax.dsf", "2147483647\n", 11);
    LexRun r2 = run_lex("test_lex_intmin.dsf", "-2147483648\n", 12);

    CHECK(r1.head != NULL);
    if (r1.head != NULL) {
        CHECK_INT(r1.head->id, T_NUMBER);
        CHECK_INT(r1.head->value, 2147483647L);
        CHECK_INT(r1.head->value, TOKEN_NO_VALUE);  /* the sentinel collision itself */
    }
    CHECK(r2.head != NULL);
    if (r2.head != NULL) {
        CHECK_INT(r2.head->id, T_NUMBER);
        CHECK_INT(r2.head->value, -2147483648LL);
    }

    run_lex_cleanup(&r1);
    run_lex_cleanup(&r2);
}

/* INT32 pinning vector: a 64-bit `long` host must reject 3000000000
   exactly as a 32-bit reference build does, not silently accept it
   via the host's native `long` width. NDRC's guard is diag_fatal
   (exit class 2), a PORT-NOTEd deviation from the reference's
   unreproducible EConvertError abort (exit 217, 19.13 live-verified
   2026-08-26). */
TEST(number_literal_over_int32_range_fatals_via_diag)
{
    LexRun r = run_lex("test_lex_intoverflow.dsf", "3000000000\n", 11);

    CHECK(r.head == NULL);
    CHECK_INT(diag_error_count(r.d), 1);
    CHECK_INT(diag_exit_code(r.d), 2);   /* diag_fatal's class */

    run_lex_cleanup(&r);
}

/* $name is T_LABEL, text keeping the leading '$' (rule 39). */
TEST(dollar_prefixed_label)
{
    LexRun r = run_lex("test_lex_label.dsf", "$foo\n", 5);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_LABEL);
        CHECK_STR(r.head->text, "$foo");
        CHECK_INT(r.head->value, TOKEN_NO_VALUE);
    }

    run_lex_cleanup(&r);
}

/* Rules 35/36: /7 is a numbered list entry (IntVal = 7, text KEEPS
   the leading '/' - lexer.pas:93); /name is a named one (IntVal =
   MaxLongInt, text has the '/' STRIPPED - lexer.pas:95). This
   text/value asymmetry between the two rules is exactly what the
   Pascal source does. */
TEST(numbered_and_named_list_entries)
{
    LexRun r1 = run_lex("test_lex_listnum.dsf", "/7\n", 3);
    LexRun r2 = run_lex("test_lex_listname.dsf", "/name\n", 6);

    CHECK(r1.head != NULL);
    if (r1.head != NULL) {
        CHECK_INT(r1.head->id, T_LIST_ENTRY);
        CHECK_STR(r1.head->text, "/7");
        CHECK_INT(r1.head->value, 7);
    }
    CHECK(r2.head != NULL);
    if (r2.head != NULL) {
        CHECK_INT(r2.head->id, T_LIST_ENTRY);
        CHECK_STR(r2.head->text, "name");
        CHECK_INT(r2.head->value, TOKEN_NO_VALUE);
    }

    run_lex_cleanup(&r1);
    run_lex_cleanup(&r2);
}

/* Rule 37: ;.*\n is discarded whole, including its newline - no
   T_COMMENT is ever produced (15.1 quirk (g)). */
TEST(semicolon_comment_consumes_to_end_of_line)
{
    LexRun r = run_lex("test_lex_comment.dsf", "; a comment, with stuff\nIDENT\n", 31);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_IDENTIFIER);
        CHECK_STR(r.head->text, "IDENT");
        CHECK(r.head->next == NULL);
    }

    run_lex_cleanup(&r);
}

/* 19.10: an embedded NUL byte acts as a silent EOF - no character
   class contains byte 0, so the driver's no-match/EOF path fires
   exactly as real end-of-file does, and everything after the NUL is
   silently discarded with no diagnostic. */
TEST(embedded_nul_is_a_silent_eof)
{
    unsigned char src[] = { 'I', 'D', 'E', 'N', 'T', '\n', 0x00, 'M', 'O', 'R', 'E', '\n' };
    LexRun r = run_lex("test_lex_nul.dsf", src, sizeof src);

    CHECK(r.head != NULL);
    if (r.head != NULL) {
        CHECK_INT(r.head->id, T_IDENTIFIER);
        CHECK_STR(r.head->text, "IDENT");
        CHECK(r.head->next == NULL);   /* "MORE" never reached */
    }
    CHECK_INT(diag_error_count(r.d), 0);   /* silent - no diagnostic at all */

    run_lex_cleanup(&r);
}

/* 19.11: FPC's readln does not treat a bare CR as a line terminator -
   it survives into the character stream and falls into the catch-all
   error rule (it IS in the DFA's wide first character class, so it
   still costs the driver a state transition before dying, exactly
   like any other invalid byte). */
TEST(lone_cr_not_followed_by_lf_is_a_lexer_error)
{
    unsigned char src[] = { 'A', 'B', 0x0D, 'C', 'D', '\n' };
    LexRun r = run_lex("test_lex_lonecr.dsf", src, sizeof src);

    CHECK(r.head == NULL);
    CHECK_INT(diag_error_count(r.d), 1);
    CHECK_INT(diag_exit_code(r.d), 1);

    run_lex_cleanup(&r);
}

/* LexerError's shape, byte-exact (USintactic.pas:49-54 via
   diag_syntax_error): "<line>:<col>:<path>: Unexpected character or
   string: "<char>".\n" - and the RAW yycolno (offending char's column
   + 1), not yycolno-1 like AddToken's stored col (19.14/15.1(h)). A
   backtick is not the start of ANY DSF.l rule, so it dies at length 1
   on the very first character of the line: col 1 (colno starts at 1)
   + 1 for the post-increment inside get_char = 2. */
TEST(lexer_error_output_is_byte_exact)
{
    LexRun r = run_lex("test_lex_errtext.dsf", "`\n", 2);
    char buf[512];
    char want[600];

    CHECK(r.head == NULL);
    diag_read(&r, buf, sizeof buf);
    snprintf(want, sizeof want,
             "1:2:%s: Unexpected character or string: \"`\".\n", r.path);
    CHECK_STR(buf, want);

    run_lex_cleanup(&r);
}

/* Defensive: lex_tokenize on a path that cannot be opened at all
   reports via diag_fatal rather than crashing - not a DRC-catalogued
   case (Preparse guarantees existence in the real pipeline), just
   this driver's own minimal robustness for a direct caller (as every
   other test in this file is). */
TEST(unopenable_path_reports_via_diag_fatal)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = tmpfile();
    Token *head;
    char path[512];

    scratch_path(path, sizeof path, "test_lex_does_not_exist.dsf");
    remove(path);
    diag_set_stream(d, f);
    head = lex_tokenize(a, d, path);

    CHECK(head == NULL);
    CHECK_INT(diag_error_count(d), 1);
    CHECK_INT(diag_exit_code(d), 2);

    fclose(f);
    arena_free(a);
}

int main(void)
{
    RUN(section_markers_lex_in_order_with_verbatim_text);
    RUN(all_directive_keywords_lex_to_their_token_ids);
    RUN(bare_if_directive_self_terminates_before_an_identifier);
    RUN(double_quoted_string_is_greedy_to_the_last_quote_on_the_line);
    RUN(lone_underscore_is_t_underscore);
    RUN(star_is_also_t_underscore);
    RUN(process_entry_sign);
    RUN(indirection_marker);
    RUN(single_quoted_string_with_one_closing_quote_is_a_lexer_error);
    RUN(single_quoted_string_with_doubled_closing_quote_is_one_string_token);
    RUN(single_quoted_string_utf8encode_matches_cp1252_not_plain_latin1);
    RUN(comma_is_an_identifier_character);
    RUN(digit_letter_run_is_identifier_but_dash_letter_is_an_error);
    RUN(negative_number_literal);
    RUN(int32_boundary_literals_both_fit);
    RUN(number_literal_over_int32_range_fatals_via_diag);
    RUN(dollar_prefixed_label);
    RUN(numbered_and_named_list_entries);
    RUN(semicolon_comment_consumes_to_end_of_line);
    RUN(embedded_nul_is_a_silent_eof);
    RUN(lone_cr_not_followed_by_lf_is_a_lexer_error);
    RUN(lexer_error_output_is_byte_exact);
    RUN(unopenable_path_reports_via_diag_fatal);
    return test_summary("lex");
}
