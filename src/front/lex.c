/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/lex.c - Copyright (C) 2026 Dan Gibson.

   PORT: lexer.pas's yylex (D:/DRC/src, branch nextdaad) - the TP Lex
   V3.0 generated driver, walking lex_tables.h's DFA (Task 1, ported
   from this same lexer.pas) - plus the "internal data structures and
   routines" half of ULexLib.pas (yynew/yyscan/yymark/yymatch/yyfind/
   yydefault/yyclear, ULexLib.pas:125-163,329-420) that Task 2
   deliberately left out of lexlib.c/h (see that file's own header
   note), plus DSF.l's 43 rule actions (lexer.pas:17-111). */

/* echo/yymore/reject/return/returnc/start (ULexLib.pas:82-107) are not
   ported: no DSF.l action calls them. yyless IS ported - yyfind uses
   it internally. */

/* Transitions use the flattened yynext layer, proven byte-identical to
   the faithful yyt/cc[8] form by exhaustive cross-check (see
   tests/test_lexconv_dump.c); yytl/yyth are still read directly for
   the dead-state test, which must run before any char is consumed. */
#include "tokenlist.h"

#include "include.h"
#include "lex_tables.h"
#include "lex_tokens.h"
#include "lexlib.h"
#include "str.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Matches ULexLib.pas's `const max_chars = 8192;` (ULexLib.pas:176) -
   the same Pascal constant lexlib.c's line buffer also keys off (that
   file's own LEXLIB_MAX_CHARS, private to lexlib.c and not shared via
   lexlib.h - duplicated here rather than exposing it, since this
   task's file scope does not extend to editing lexlib.h beyond
   un-inerting yytext/yyleng). Bounds yyscan's per-TOKEN length guard,
   a different mechanism from lexlib.c's own per-LINE guard (19.12). */
#define LEX_MAX_CHARS 8192

/* Matches ULexLib.pas's `max_matches = 1024;` (ULexLib.pas:248) - the
   match stack's capacity; a token around 1020+ characters overflows
   this before LEX_MAX_CHARS is reached (19.12). Rules are 1..43
   (LEX_MAX_RULES sized one past 43 so rule numbers can index it
   directly, matching Pascal's 1-based yypos). */
#define LEX_MAX_MATCHES 1024
#define LEX_MAX_RULES 44

/* Driver state, file-scope like every ULexLib.pas/lexer.pas global
   this ports (PORT NOTE, matching lexlib.c's own precedent: DRC
   compiles once per process, so this never needed to be
   instantiable). Reset at the top of every lex_tokenize call, so
   repeated calls within one process (as this file's own tests make)
   each start a fresh, independent scan. */
static char g_yytext_buf[LEX_MAX_CHARS + 1];
static int g_yystate;                        /* yystate */
static int g_yylstate;                       /* yylstate: 1 at BOL/start, 0 mid-line */
static unsigned char g_yylastchar;            /* yylastchar, 0 = none yet */
static int g_yypos[LEX_MAX_RULES];            /* per-rule marked length; 0 = unmarked */
static int g_yystack[LEX_MAX_MATCHES + 1];    /* match stack, 1-based like Pascal */
static int g_yymatches;

static void lex_fatal(const char *msg)
{
    /* PORT: ULexLib.pas's fatal() (ULexLib.pas:165-170) - same shape
       lexlib.c's own lexlib_fatal reproduces (that copy is
       file-static there and not shared via lexlib.h, hence this
       second, deliberately identical, copy - see the file header). */
    fprintf(stderr, "LexLib: %s\n", msg);
    exit(1);
}

/* PORT: ULexLib.pas:344-350. Reads one character, appends it to
   yytext/yyleng, and returns it (Pascal's own yyactchar is this
   return value, kept local to the scan loop below rather than a
   separate global - nothing outside this loop ever reads it). */
static unsigned char yyscan(void)
{
    unsigned char c;

    if (yyleng == LEX_MAX_CHARS)
        lex_fatal("Lexer overflow, this usually means a line in your source file is too long");

    c = get_char();
    g_yytext_buf[yyleng] = (char)c;
    yyleng++;
    g_yytext_buf[yyleng] = '\0';
    yytext = g_yytext_buf;
    return c;
}

/* PORT: ULexLib.pas:278-289 (yyless).
   Reference bug NOT reproduced (ULexLib.pas:287): its yyless never
   trims yyleng, so yylastchar reads #0 and yylstate freezes. Provably
   inert for DSF.l (table rows for states 0 and 1 are byte-identical)
   but a silent hazard if the tables are regenerated - this port keeps
   yyleng in sync. */
static void yyless(int n)
{
    int i;
    for (i = (int)yyleng; i > n; i--)
        unget_char((unsigned char)g_yytext_buf[i - 1]);
    yyleng = (unsigned)n;
    g_yytext_buf[n] = '\0';
    yytext = g_yytext_buf;
}

/* PORT: ULexLib.pas:329-342 (yynew). yysstate is always 0 - DSF.l
   defines no start conditions, so start() is never called - so
   yystate reduces to yylstate alone. */
static void yynew(void)
{
    if (g_yylastchar != 0)
        g_yylstate = (g_yylastchar == '\n') ? 1 : 0;
    g_yystate = g_yylstate;
    yyleng = 0;
    g_yytext_buf[0] = '\0';
    yytext = g_yytext_buf;
    g_yymatches = 0;
}

/* PORT: ULexLib.pas:352-356 (yymark).
   Pascal's 'too many rules' guard is not ported: n only ever comes
   from the generated yyk/yym tables (1..43), never from input. */
static void yymark(int n)
{
    g_yypos[n] = (int)yyleng;
}

/* PORT: ULexLib.pas:358-363 (yymatch). */
static void yymatch(int n)
{
    g_yymatches++;
    if (g_yymatches > LEX_MAX_MATCHES) lex_fatal("match stack overflow");
    g_yystack[g_yymatches] = n;
}

/* PORT: ULexLib.pas:365-392 (yyfind). */
static int yyfind(int *rule_out)
{
    int n;

    while (g_yymatches > 0 && g_yypos[g_yystack[g_yymatches]] == 0)
        g_yymatches--;

    if (g_yymatches > 0) {
        n = g_yystack[g_yymatches];
        yyless(g_yypos[n]);
        g_yypos[n] = 0;
        g_yylastchar = (yyleng > 0) ? (unsigned char)g_yytext_buf[yyleng - 1] : 0;
        *rule_out = n;
        return 1;
    }

    yyless(0);
    g_yylastchar = 0;
    return 0;
}

/* PORT: ULexLib.pas:394-409 (yydefault), minus the put_char echo
   (dead code for a compiler front end, matching lexlib.c's own
   precedent - see this file's header). For THIS grammar's tables,
   only ever reached when yyfind failed, which per section 27.1's
   "#0 in no cc set / all values #1..#255 are covered from the start
   states" fact can only happen at true EOF or an embedded NUL
   (19.10) - so the "more input available" branch below is provably
   dead for DSF.l specifically, but is still implemented for driver
   fidelity to the Pascal control flow (section 27.2/lexer.pas's own
   action: label). */
static int yydefault(void)
{
    unsigned char c = get_char();
    if (c != 0) {
        g_yylastchar = c;
        return 1;
    }
    g_yylstate = 1;
    g_yylastchar = 0;
    return 0;
}

/* PORT: ULexLib.pas:411-420 (yyclear), the subset relevant to this
   driver's own state (the input buffer itself is lexlib.c's, reset by
   lexlib_close/lexlib_open instead). */
static void yyclear(void)
{
    g_yylstate = 1;
    g_yylastchar = 0;
    yyleng = 0;
    g_yytext_buf[0] = '\0';
    yytext = g_yytext_buf;
}

/* PORT NOTE (19.13): parses via int64 with an explicit int32 range
   check - strtol on a 64-bit long would accept 3000000000 where the
   reference's 32-bit Longint crashes with EConvertError, exit 217
   (verified 2026-08-26 against D:/DRC/src/drf.exe). The caller
   reports via diag_fatal instead of crashing. */
static int parse_int32(const char *text, size_t len, long *out)
{
    long long acc = 0;
    size_t i = 0;
    int neg = 0;

    if (len > 0 && text[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i >= len) return 0; /* defensive: the DFA never produces a bare sign */

    for (; i < len; i++) {
        acc = acc * 10 + (long long)(text[i] - '0');
        if (acc > 2147483648LL) return 0;
    }
    if (neg) acc = -acc;
    if (acc < -2147483648LL || acc > 2147483647LL) return 0;
    *out = (long)acc;
    return 1;
}

/* PORT NOTE (UTF8Encode, rule 31 only - lexer.pas:85/DSF.l:45; 15.1
   quirk (c)). FPC's UTF8Encode treats source bytes as CP1252, not
   ISO-8859-1 (no $codepage directive; the AnsiString default tag
   decides). Pinned per-byte against the reference build (D:/DRC/src/
   drf.exe, branch nextdaad) on 2026-08-27, undefined CP1252 slots
   behaving as identity (WHATWG windows-1252); the byte-level pins
   live in tests/test_lex.c. Only rule 31 runs this; rule 30 does
   not. */
static const unsigned short cp1252_0x80_0x9f[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static void append_utf8_cp1252_byte(Str *out, unsigned char b)
{
    unsigned cp;

    if (b < 0x80) {
        str_push(out, (char)b);
        return;
    }
    cp = (b < 0xA0) ? cp1252_0x80_0x9f[b - 0x80] : (unsigned)b;

    if (cp < 0x800) {
        str_push(out, (char)(0xC0 | (cp >> 6)));
        str_push(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        str_push(out, (char)(0xE0 | (cp >> 12)));
        str_push(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        str_push(out, (char)(0x80 | (cp & 0x3F)));
    }
}

static const char *utf8_encode_cp1252(Arena *a, const char *text, size_t len)
{
    Str *s = str_new(a);
    size_t i;
    for (i = 0; i < len; i++)
        append_utf8_cp1252_byte(s, (unsigned char)text[i]);
    return arena_strdup(a, str_cstr(s));
}

static void add_simple(TokenList *list, Arena *a, int id)
{
    tokenlist_add(list, a, id, arena_strndup(a, yytext, yyleng),
                  TOKEN_NO_VALUE, yylineno, yycolno);
}

/* PORT: lexer.pas:19-111 (yyaction), the 43 DSF.l rule actions,
   hand-ported one for one. Returns 0 (and has already reported via
   diag) exactly when tokenization must stop immediately: rule 43's
   catch-all (LexerError's shape) or an out-of-int32 literal (rules 35
   and 38, the only two that ever compute a real IntVal). */
static int lex_action(Arena *a, Diag *d, TokenList *list, int rule)
{
    switch (rule) {
    case 1: add_simple(list, a, T_SECTION_CTL); break;
    case 2: add_simple(list, a, T_SECTION_STX); break;
    case 3: add_simple(list, a, T_SECTION_CON); break;
    case 4: add_simple(list, a, T_SECTION_MTX); break;
    case 5: add_simple(list, a, T_SECTION_LTX); break;
    case 6: add_simple(list, a, T_SECTION_OTX); break;
    case 7: add_simple(list, a, T_SECTION_OBJ); break;
    case 8: add_simple(list, a, T_SECTION_VOC); break;
    case 9: add_simple(list, a, T_SECTION_PRO); break;
    case 10: add_simple(list, a, T_SECTION_END); break;
    case 11: add_simple(list, a, T_DEFINE); break;
    case 12: add_simple(list, a, T_IFDEF); break;         /* #ifdef */
    case 13: add_simple(list, a, T_IFDEF); break;         /* #if alias - 19.9/15.1(a) */
    case 14: add_simple(list, a, T_IFNDEF); break;
    case 15: add_simple(list, a, T_ENDIF); break;
    case 16: add_simple(list, a, T_ELSE); break;
    case 17: add_simple(list, a, T_ECHO); break;
    case 18: add_simple(list, a, T_USERPTR); break;
    case 19: add_simple(list, a, T_INT); break;
    case 20: add_simple(list, a, T_SFX); break;
    case 21: add_simple(list, a, T_HEX); break;
    case 22: add_simple(list, a, T_DEBUG); break;
    case 23: add_simple(list, a, T_DB); break;            /* #db */
    case 24: add_simple(list, a, T_DB); break;            /* #defb alias */
    case 25: add_simple(list, a, T_DW); break;            /* #dw */
    case 26: add_simple(list, a, T_DW); break;            /* #defw alias */
    case 27: add_simple(list, a, T_EXTERN); break;
    case 28: add_simple(list, a, T_INCBIN); break;
    case 29: add_simple(list, a, T_CLASSIC); break;
    case 30: add_simple(list, a, T_STRING); break;        /* "..." greedy to last quote, 19.7 */
    case 31: {                                            /* '...''  - UTF8Encode, 19.6 */
        const char *encoded = utf8_encode_cp1252(a, yytext, yyleng);
        tokenlist_add(list, a, T_STRING, encoded, TOKEN_NO_VALUE, yylineno, yycolno);
        break;
    }
    case 32: add_simple(list, a, T_UNDERSCORE); break;    /* "_" */
    case 33: add_simple(list, a, T_UNDERSCORE); break;    /* "*" */
    case 34: add_simple(list, a, T_PROCESS_ENTRY_SIGN); break; /* ">" */
    case 35: {                                            /* /digits - numbered list entry */
        long value;
        /* PORT: lexer.pas:93 stores the FULL yytext (leading '/'
           included) as Text, but parses IntVal from
           Copy(yytext,2,Length-1) (the slash stripped) - this
           text/value asymmetry against rule 36 below is exactly what
           the Pascal source does, not a transcription slip. */
        if (!parse_int32(yytext + 1, yyleng - 1, &value)) {
            diag_fatal(d, "list entry number \"%s\" is out of range for a 32-bit integer", yytext);
            return 0;
        }
        tokenlist_add(list, a, T_LIST_ENTRY, arena_strndup(a, yytext, yyleng),
                      value, yylineno, yycolno);
        break;
    }
    case 36:                                              /* /name - named list entry */
        /* PORT: lexer.pas:95 - Text is Copy(yytext,2,Length-1) (slash
           stripped), IntVal is MaxLongInt (no numeric value). */
        tokenlist_add(list, a, T_LIST_ENTRY, arena_strndup(a, yytext + 1, yyleng - 1),
                      TOKEN_NO_VALUE, yylineno, yycolno);
        break;
    case 37: break;                                       /* ;comment\n - discarded, 15.1(g) */
    case 38: {                                             /* -?[0-9]+ */
        long value;
        if (!parse_int32(yytext, yyleng, &value)) {
            diag_fatal(d, "number literal \"%s\" is out of range for a 32-bit integer", yytext);
            return 0;
        }
        tokenlist_add(list, a, T_NUMBER, arena_strndup(a, yytext, yyleng),
                      value, yylineno, yycolno);
        break;
    }
    case 39: add_simple(list, a, T_LABEL); break;         /* $name */
    case 40: add_simple(list, a, T_IDENTIFIER); break;
    case 41: add_simple(list, a, T_INDIRECT); break;      /* @ */
    case 42: break;                                       /* [ \t\n] - discarded */
    case 43: {                                             /* catch-all - 15.1(h) */
        /* PORT: USintactic.pas:49-54 (LexerError). Passes the RAW
           yycolno (offending char's column + 1), not yycolno-1 like a
           stored token's colno. include_remap_for_diag maps a
           #include'd line back to the original file (SyntaxError /
           GetIncludeData, USintactic.pas:34-38). */
        long mapped = include_remap_for_diag(d, yylineno);
        diag_syntax_error(d, (int)mapped, yycolno,
                           "Unexpected character or string: \"%s\"", yytext);
        return 0;
    }
    default:
        break; /* unreachable: yyfind only ever returns 1..43 */
    }
    return 1;
}

Token *lex_tokenize(Arena *a, Diag *d, const char *path)
{
    TokenList *list;
    int rule;

    diag_set_source(d, path);

    if (!lexlib_open(path)) {
        diag_fatal(d, "cannot open \"%s\"", path);
        return NULL;
    }

    /* PORT: ULexLib.pas's unit initialization (ULexLib.pas:422-428,
       "begin yywrap:=...; yyclear end.") ran once before drf.pas's
       first use; reproduced here so repeated lex_tokenize calls
       within one process each start a clean scan. */
    g_yylstate = 1;
    g_yylastchar = 0;
    g_yymatches = 0;
    memset(g_yypos, 0, sizeof g_yypos);
    yytext = g_yytext_buf;
    yyleng = 0;
    g_yytext_buf[0] = '\0';

    list = tokenlist_new(a);

    /* PORT: lexer.pas:1727-1780 (yylex's start/scan/action labels),
       transcribed per section 27.2. */
    for (;;) {
        yynew();

        /* scan: */
        for (;;) {
            int i, lo = yykl[g_yystate], hi = yykh[g_yystate];
            int16_t next;
            unsigned char c;

            for (i = lo; i <= hi; i++) yymark(yyk[i]);
            for (i = yymh[g_yystate]; i >= yyml[g_yystate]; i--) yymatch(yym[i]);

            if (yytl[g_yystate] > yyth[g_yystate]) break; /* dead state -> action */

            c = yyscan();
            next = yynext[g_yystate][c];
            if (next < 0) break; /* no transition on c -> action */
            g_yystate = next;
        }

        /* action: */
        if (yyfind(&rule)) {
            if (!lex_action(a, d, list, rule)) {
                lexlib_close();
                return NULL;
            }
            /* Pascal: "if yyreject then goto action" - none of the 43
               actions ever calls reject(), so yyreject is always
               false here; the REJECT loop is correctly omitted. */
        } else if (!yydefault()) {
            /* yywrap() always returns true in this port (there is no
               yyoutput to close, unlike ULexLib.pas's default
               yywrap - 19.16) - true EOF or an embedded NUL both
               reach here identically (19.10). */
            yyclear();
            break;
        }
        /* Dead for DSF.l's tables (see yydefault); kept for driver fidelity. */
    }

    lexlib_close();
    return tokenlist_head(list);
}
