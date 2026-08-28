/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_expr.c - Copyright (C) 2026 Dan Gibson.

   Replays the engine-owned probes of DRF's expression battery against
   src/front/expr.c; the rest replays at the caller. Float results
   apply the caller's trunc() toward zero (USintactic.pas:135); error
   probes carry the INNER exception text only, without the caller's
   lead-in/period. */
#include "test.h"

#include "../src/front/expr.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Which symbol table a probe is evaluated against. NEXTDAAD's COLS is
   80 (drf.pas:126,248) and the fixture has one object, so NUM_OBJECTS
   is 1 (USintactic.pas:361-362). The two shadow sets add the #define
   that steals a bcMath name (19.58). */
typedef enum { SYM_BASE, SYM_PI, SYM_SQRT } SymSet;

typedef enum {
    W_INT,      /* EXPR_INT, ival == want */
    W_FLOAT,    /* EXPR_FLOAT, trunc(fval) == want */
    W_NONNUM,   /* EXPR_NONNUM, no message */
    W_FAIL      /* EXPR_FAIL, msg == want_msg */
} Want;

typedef struct {
    const char *probe;
    const char *expr;
    SymSet      syms;
    Want        want;
    long long   want_val;
    const char *want_msg;
} Case;

/* Value probes P1-P22, P24-P26, P28-P33 of the DRF expression battery,
   every value read back from reference DRF 0.40 (target NEXTDAAD, -v3).
   P23, P27 and P34 are caller-side and are not replayed here: P23 is
   the ShortString truncation of USintactic.pas:124-125, P27 the
   indirected condact operand, P34 the #db leg. */
static const Case VALUE_CASES[] = {
    { "P1",  "1+2",                   SYM_BASE, W_INT,   3,          NULL },
    { "P2",  "2+3*4",                 SYM_BASE, W_INT,   14,         NULL },
    { "P3",  "7/2",                   SYM_BASE, W_FLOAT, 3,          NULL },
    { "P4",  "6/2",                   SYM_BASE, W_FLOAT, 3,          NULL },
    { "P5",  "7 mod 2",               SYM_BASE, W_INT,   1,          NULL },
    { "P6",  "2--3",                  SYM_BASE, W_INT,   5,          NULL },
    { "P7",  "(1+2)*3",               SYM_BASE, W_INT,   9,          NULL },
    { "P8",  "COLS+1",                SYM_BASE, W_FLOAT, 81,         NULL },
    { "P9",  "cols+1",                SYM_BASE, W_FLOAT, 81,         NULL },
    { "P10", "sqrt(16)",              SYM_BASE, W_FLOAT, 4,          NULL },
    { "P11", "trunc(3.9)",            SYM_BASE, W_INT,   3,          NULL },
    { "P12", "pi",                    SYM_BASE, W_FLOAT, 3,          NULL },
    { "P13", "SQRT(9)",               SYM_BASE, W_FLOAT, 3,          NULL },
    { "P14", "2^3",                   SYM_BASE, W_FLOAT, 8,          NULL },
    { "P15", "$FF",                   SYM_BASE, W_INT,   255,        NULL },
    { "P16", "%101",                  SYM_BASE, W_INT,   5,          NULL },
    { "P17", "&17",                   SYM_BASE, W_INT,   15,         NULL },
    { "P18", "1.5+1.5",               SYM_BASE, W_FLOAT, 3,          NULL },
    { "P19", "1e2",                   SYM_BASE, W_FLOAT, 100,        NULL },
    { "P20", "12 and 10",             SYM_BASE, W_INT,   8,          NULL },
    { "P21", "1",                     SYM_BASE, W_INT,   1,          NULL },
    { "P22", "NUM_OBJECTS",           SYM_BASE, W_FLOAT, 1,          NULL },
    { "P24", "trunc(3)",              SYM_BASE, W_INT,   3,          NULL },
    { "P25", "if(1=1,5,6)",           SYM_BASE, W_INT,   5,          NULL },
    { "P26", "case(2,99,1,10,2,20)",  SYM_BASE, W_INT,   20,         NULL },
    { "P28", "4000000000-2000000000", SYM_BASE, W_INT,   2000000000, NULL },
    { "P29", "-5",                    SYM_BASE, W_INT,   -5,         NULL },
    { "P30", "-7/2",                  SYM_BASE, W_FLOAT, -3,         NULL },
    { "P31", "not 0",                 SYM_BASE, W_INT,   -1,         NULL },
    { "P32", "pi",                    SYM_PI,   W_FLOAT, 5,          NULL },
    { "P33", "sqrt+1",                SYM_SQRT, W_FLOAT, 4,          NULL }
};

/* Error probes P35-P39, P41, P42, P45, P49-P56. P40, P43, P44, P46-P48
   are caller-side (parameter range check, Longint range crash,
   MaxLongInt sentinel) and are not replayed here.

   P49's operand reaches the engine as `1+2'`: the DSF token is
   `'1+2''`, and the caller strips one character from each end. */
static const Case ERROR_CASES[] = {
    { "P35", "7 div 2",   SYM_BASE, W_FAIL, 0,
      "Badly terminated expression. Found token at position 6 : div" },
    { "P36", "1=1",       SYM_BASE, W_NONNUM, 0, NULL },
    { "P37", "nosuch+1",  SYM_BASE, W_FAIL, 0,
      "Unknown identifier: nosuch" },
    { "P38", "1++",       SYM_BASE, W_FAIL, 0,
      "Unknown token at pos 4 : " },
    { "P39", "",          SYM_BASE, W_FAIL, 0,
      "Cannot evaluate: empty expression" },
    { "P41", "1/0",       SYM_BASE, W_FAIL, 0,
      "rtInteger division by zero" },
    { "P42", "1.0/0",     SYM_BASE, W_FAIL, 0,
      "rtInteger division by zero" },
    { "P45", "1 2",       SYM_BASE, W_FAIL, 0,
      "Badly terminated expression. Found token at position 4 : 2" },
    { "P49", "1+2'",      SYM_BASE, W_FAIL, 0,
      "Unexpected character in number : '" },
    { "P50", "true",      SYM_BASE, W_NONNUM, 0, NULL },
    { "P51", "'abc'",     SYM_BASE, W_NONNUM, 0, NULL },
    { "P52", "1.5 and 1", SYM_BASE, W_FAIL, 0,
      "Node type (rtFloat) not in allowed types (rtBoolean,rtInteger) "
      "for expression:  1.5000000000000000E+000" },
    { "P53", "sqrt(-1)",  SYM_BASE, W_FAIL, 0,
      "Invalid floating point operation" },
    { "P54", "sqrt(16)",  SYM_SQRT, W_FAIL, 0,
      "Badly terminated expression. Found token at position 6 : (" },
    { "P55", "shl(1,3)",  SYM_BASE, W_FAIL, 0,
      "Unknown identifier: shl" },
    { "P56", "COLS mod 2", SYM_BASE, W_FAIL, 0,
      "Node type (rtFloat) not in allowed types (rtInteger) "
      "for expression: COLS" }
};

/* Regression guards, NOT battery probes: each pins a reference DRF
   0.40 quirk no assigned probe distinguishes, so a later "fix" turns
   the suite red. Every value read back from drf.exe (NEXTDAAD -v3,
   2026-08-27), same method as the battery.
   R1        ^ is observably RIGHT associative: the Pascal loop's right
             operand re-enters Level5 (fpexprpars.pp:1923) and swallows
             the chain. 2^3^2 = 512, not 64.
   R2-R3     Check runs on the ROOT node only (fpexprpars.pp:2075);
             TFPBinaryAndOperation.GetNodeValue's boolean/integer arms
             have no else (fpexprpars.pp:2925-2937), so type errors
             under a well-typed root compile (no P52).
   R4-R6     TFPExpressionResult is a variant record (fpexprpars.pp:
             91-100): int next to a float VARIABLE reads the Double's
             bit pattern as Int64 (COLS = 80.0 = 0x4054000000000000).
             R4-R5 alias via Level1, which does NOT promote; R6 aliases
             via Level4, which DOES (fpexprpars.pp:1780-1793) - the
             promoted 1000000.0 is read back as an Int64 too.
   R7-R19    ResBoolean is ONE byte at offset 0 of the shared slot;
             boolean ops are BITWISE on that byte, not logical, and a
             comparison's write touches byte 0 only over whatever the
             slot held (comparisons fill Result from the LEFT operand
             first, fpexprpars.pp:3330-3344); `not` NORMALISES its
             operand's byte (0x54 and 0x55 both come back false).
   R20-R21   Boolean compare is a raw byte compare (fpexprpars.pp:3299,
             3143): 0x01 vs 0x54 are both true yet unequal; an
             unmatched case tag with even argc falls to FArgs[1]
             (fpexprpars.pp:3156-3158). */
static const Case GUARD_CASES[] = {
    { "R1", "2^3^2",              SYM_BASE, W_FLOAT, 512,  NULL },
    { "R2", "(1.5 and 1)+0",      SYM_BASE, W_FLOAT, 1,    NULL },
    { "R3", "(1.5 and 1)*2",      SYM_BASE, W_FLOAT, 3,    NULL },
    { "R4", "(1 and COLS)+0",     SYM_BASE, W_INT,   0,    NULL },
    { "R5", "(1 or COLS)+0",      SYM_BASE, W_INT,
      4635329916471083009LL, NULL },
    { "R6", "(COLS mod 1000000)+0", SYM_BASE, W_INT,
      4635329916471083008LL, NULL },
    { "R7",  "if(1+1/1099511627776,1,2)+0",              SYM_BASE,
      W_INT, 2, NULL },
    { "R8",  "if((1=1) and (1+1/1099511627776),1,2)+0",  SYM_BASE,
      W_INT, 2, NULL },
    { "R9",  "if((1=0) or (1+1/1099511627776),1,2)+0",   SYM_BASE,
      W_INT, 2, NULL },
    { "R10", "if((1=1) xor (1+1/1099511627776),1,2)+0",  SYM_BASE,
      W_INT, 1, NULL },
    { "R11", "if((1=0)=(1+1/1099511627776),1,2)+0",      SYM_BASE,
      W_INT, 1, NULL },
    { "R12", "if((1=1) and (1+84/4503599627370496),1,2)+0", SYM_BASE,
      W_INT, 2, NULL },
    { "R13", "if(1+84/4503599627370496,1,2)+0",          SYM_BASE,
      W_INT, 1, NULL },
    { "R14", "if(((1=0) or (1+84/4503599627370496)) and (1=1),1,2)+0",
      SYM_BASE, W_INT, 2, NULL },
    { "R15", "if(not ((1=0) or (1+84/4503599627370496)),1,2)+0",
      SYM_BASE, W_INT, 2, NULL },
    { "R16", "case((300>1),9,(1=1),5,(1=0),6)+0",        SYM_BASE,
      W_INT, 5, NULL },
    { "R17", "(1 or (300>1))+0",                         SYM_BASE,
      W_INT, 257, NULL },
    { "R18", "((1 or (257=0))-257)+0",                   SYM_BASE,
      W_INT, 0, NULL },
    { "R19", "((1 or (1.5<2))-4609434218613702657)+0",   SYM_BASE,
      W_INT, 0, NULL },
    { "R20", "if((1=1)=(1+84/4503599627370496),1,2)+0",  SYM_BASE,
      W_INT, 2, NULL },
    { "R21", "case((1=1),9,(1+84/4503599627370496),5,(1=0),6)+0",
      SYM_BASE, W_INT, 9, NULL }
};

static const char *kind_name(ExprKind k)
{
    switch (k) {
    case EXPR_INT:    return "EXPR_INT";
    case EXPR_FLOAT:  return "EXPR_FLOAT";
    case EXPR_NONNUM: return "EXPR_NONNUM";
    case EXPR_FAIL:   return "EXPR_FAIL";
    }
    return "?";
}

static SymbolList *build_symbols(Arena *a, Diag *d, SymSet which)
{
    SymbolList *sl = symbols_new(a);

    symbols_add(sl, a, d, "COLS", 80);
    symbols_add(sl, a, d, "NUM_OBJECTS", 1);
    if (which == SYM_PI)   symbols_add(sl, a, d, "PI", 5);
    if (which == SYM_SQRT) symbols_add(sl, a, d, "SQRT", 3);
    return sl;
}

/* Reports against the PROBE id rather than the enclosing test name, so
   a red run names the row of 29.9 that moved. */
static void probe_fail(const Case *tc, const char *fmt, ...)
{
    va_list ap;

    ndrc_tests_failed++;
    printf("FAIL %s \"%s\": ", tc->probe, tc->expr);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void run_case(const Case *tc)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    SymbolList *sl = build_symbols(a, d, tc->syms);
    ExprResult r;
    ExprKind want_kind;

    expr_evaluate(tc->expr, sl, a, &r);

    want_kind = (tc->want == W_INT)    ? EXPR_INT
              : (tc->want == W_FLOAT)  ? EXPR_FLOAT
              : (tc->want == W_NONNUM) ? EXPR_NONNUM
                                       : EXPR_FAIL;
    if (r.kind != want_kind) {
        probe_fail(tc, "kind %s, want %s (msg \"%s\")",
                   kind_name(r.kind), kind_name(want_kind),
                   (r.kind == EXPR_FAIL) ? r.msg : "");
        arena_free(a);
        return;
    }

    switch (tc->want) {
    case W_INT:
        if ((long long)r.ival != tc->want_val)
            probe_fail(tc, "ival %lld, want %lld", (long long)r.ival,
                       tc->want_val);
        break;
    case W_FLOAT:
        /* The caller's trunc() toward zero, USintactic.pas:135. */
        if ((long long)trunc(r.fval) != tc->want_val)
            probe_fail(tc, "trunc(fval) %lld, want %lld (fval %.17g)",
                       (long long)trunc(r.fval), tc->want_val, r.fval);
        break;
    case W_NONNUM:
        if (r.msg[0] != '\0')
            probe_fail(tc, "EXPR_NONNUM carries msg \"%s\"", r.msg);
        break;
    case W_FAIL:
        if (strcmp(r.msg, tc->want_msg) != 0)
            probe_fail(tc, "msg\n     got  \"%s\"\n     want \"%s\"",
                       r.msg, tc->want_msg);
        break;
    }
    arena_free(a);
}

/* In the reference every symbol is a float VARIABLE and `/` is always
   float division; these probes pin the result KIND, not just the
   number - P4 (6/2) and P8 (COLS+1) are floats that land on integers. */
TEST(value_probes_replay)
{
    size_t i;

    for (i = 0; i < sizeof VALUE_CASES / sizeof VALUE_CASES[0]; i++)
        run_case(&VALUE_CASES[i]);
}

TEST(error_probes_replay)
{
    size_t i;

    for (i = 0; i < sizeof ERROR_CASES / sizeof ERROR_CASES[0]; i++)
        run_case(&ERROR_CASES[i]);
}

TEST(quirk_regression_guards)
{
    size_t i;

    for (i = 0; i < sizeof GUARD_CASES / sizeof GUARD_CASES[0]; i++)
        run_case(&GUARD_CASES[i]);
}

/* (d) A NULL symbol list is the documented "no variables registered"
   case: arithmetic still works, and every identifier that is not a
   bcMath builtin is unknown. */
TEST(null_symbol_list_evaluates_with_builtins_only)
{
    Arena *a = arena_new(0);
    ExprResult r;

    expr_evaluate("1+2", NULL, a, &r);
    CHECK_INT(r.kind, EXPR_INT);
    CHECK_INT(r.ival, 3);

    expr_evaluate("sqrt(16)", NULL, a, &r);
    CHECK_INT(r.kind, EXPR_FLOAT);
    CHECK_INT((long long)trunc(r.fval), 4);

    expr_evaluate("COLS", NULL, a, &r);
    CHECK_INT(r.kind, EXPR_FAIL);
    CHECK_STR(r.msg, "Unknown identifier: COLS");

    arena_free(a);
}

int main(void)
{
    RUN(value_probes_replay);
    RUN(error_probes_replay);
    RUN(quirk_regression_guards);
    RUN(null_symbol_list_evaluates_with_builtins_only);
    return test_summary("expr");
}
