/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/expr.c - Copyright (C) 2026 Dan Gibson.

   PORT: fpexprpars.pp (FPC 3.2.2), the reachable surface only - see
   expr.h for what belongs to the caller instead.

   Structure mirrors the Pascal one for one so a reader can diff the two:
   a scanner (fpexprpars.pp:1212-1499), a seven-level recursive descent
   (1762-2059), a per-node Check (called on the ROOT ONLY, exactly as
   SetExpression does at fpexprpars.pp:2075 - a type error buried under a
   passing root node is NOT diagnosed there, and must not be here), and
   an evaluator (the GetNodeValue methods). Error texts are the
   Resourcestring block at fpexprpars.pp:858-899, formatted the same way.

   Three things are load-bearing and easy to "improve" by accident:

   1. Every DSF symbol is a float variable, so a symbol reference floats
      its whole subtree - that is why "COLS mod 2" is a type error.
   2. `^` is a left-associative LOOP whose right operand re-enters level
      5 (fpexprpars.pp:1920-1926) and falls through to level 6, so it
      swallows the rest of the power chain: 2^3^2 is 512, not 64.
   3. `^` is exp(y*ln(x)) with a negative-base fixup
      (fpexprpars.pp:3613-3630), not pow() - different rounding and
      different faults: (-8)^(1/3) is a fault, not -2.

   Integer arithmetic is unchecked Int64 (the unit sets no $R/$Q,
   fpexprpars.pp:16-17), reproduced here as uint64 arithmetic cast back
   so the wrap is defined rather than C signed-overflow UB.

   The reference runs with invalid/zero-divide/overflow unmasked; this
   port raises at the faulting op with the same texts. One divergence:
   a final-result overflow the reference lets escape to the caller's
   trunc() crash (same shape as 19.55, see sintactic.c) is raised here
   as an ordinary failure; no expression that reaches it can appear in
   a working DSF, since the reference cannot compile one. */

#include "expr.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
   Result types - TResultType (fpexprpars.pp:88). The DECLARATION ORDER
   is observable: CheckNodeType lists the allowed set by walking
   Low(TResultType)..High(TResultType) (fpexprpars.pp:2958-2963), which
   is why "(rtBoolean,rtInteger)" comes out in that order.
   ------------------------------------------------------------------ */
typedef enum {
    RT_BOOLEAN = 0,
    RT_INTEGER,
    RT_FLOAT,
    RT_DATETIME,
    RT_STRING,
    RT_CURRENCY
} ResType;

#define RT_COUNT 6

/* ResultTypeName (fpexprpars.pp:921-925) - GetEnumName, so the Pascal
   identifier spelling verbatim. */
static const char *const RT_NAMES[RT_COUNT] = {
    "rtBoolean", "rtInteger", "rtFloat", "rtDateTime", "rtString",
    "rtCurrency"
};

/* TResultTypes, a Pascal set, as a bitmask. */
#define TSET(t) (1u << (unsigned)(t))

#define SET_BOOL_INT      (TSET(RT_BOOLEAN) | TSET(RT_INTEGER))
#define SET_INT           (TSET(RT_INTEGER))
#define SET_NUM           (TSET(RT_INTEGER) | TSET(RT_CURRENCY) | \
                           TSET(RT_FLOAT))
#define SET_NEGATABLE     (TSET(RT_INTEGER) | TSET(RT_FLOAT) | \
                           TSET(RT_CURRENCY))
#define SET_MATH          (TSET(RT_INTEGER) | TSET(RT_FLOAT) | \
                           TSET(RT_CURRENCY) | TSET(RT_DATETIME) | \
                           TSET(RT_STRING))
#define SET_SUB           (TSET(RT_INTEGER) | TSET(RT_FLOAT) | \
                           TSET(RT_CURRENCY) | TSET(RT_DATETIME))

/* TFPExpressionResult (fpexprpars.pp:91-100) is a variant record: one
   8-byte slot shared by ResInteger/ResFloat/..., copied whole by
   GetNodeValue (fpexprpars.pp:3759-3765). ResBoolean is ONE byte at
   offset 0 (FPC little-endian x86); the bool/int evaluator arms have
   no else, so cross-type reads see raw bit patterns - the root-only
   Check (see check_root) lets that happen on inputs the reference
   compiles successfully - and comparison nodes fill Result from Left
   before writing ResBoolean (fpexprpars.pp:3330-3344), so boolean ops
   are bitwise on byte 0 over whatever Left left there. All pinned
   against reference DRF 0.40 (NEXTDAAD -v3, 2026-08-27); the probe
   expressions live in tests/test_expr.c (GUARD_CASES). An anonymous
   union reproduces this as defined behaviour on any little-endian
   target. */
typedef struct {
    ResType     type;
    const char *s;      /* ResString - separate storage */
    union {
        uint8_t  b;     /* ResBoolean - Boolean, ONE byte at offset 0    */
        int64_t  i;     /* ResInteger                                    */
        double   f;     /* ResFloat, and ResDateTime, which is a Double  */
        int64_t  cur;   /* ResCurrency - an Int64 of ten-thousandths     */
    };
} Val;

/* The only PARTIAL write in the evaluator, and it is deterministic:
   Pascal stores one byte into ResBoolean and leaves the other seven
   holding whatever the record carried before - at the const site a
   freshly allocated (zeroed) TFPConstExpression.FValue, at every
   comparison site the left operand's own bytes. node_new's arena_calloc
   supplies the first; writing only the byte supplies the second. */
static void val_bool(Val *v, int b)
{
    v->b = (uint8_t)(b != 0);
    v->type = RT_BOOLEAN;
}

/* ------------------------------------------------------------------
   Error texts - Resourcestring, fpexprpars.pp:858-899. Spacing is
   byte-exact: several have a space before the colon.
   ------------------------------------------------------------------ */
#define S_UNKNOWN_CHARACTER   "Unknown character at pos %ld: \"%c\""
#define S_UNKNOWN_TOKEN_AT    "Unknown token at pos %ld : %s"
#define S_BAD_QUOTES          "Unterminated string"
#define S_UNKNOWN_DELIMITER   "Unknown delimiter character: \"%c\""
#define S_UNEXPECTED_EOE      "Unexpected end of expression"
#define S_UNKNOWN_COMPARISON  "Internal error: Unknown comparison"
#define S_UNKNOWN_BOOLEAN_OP  "Internal error: Unknown boolean operation"
#define S_BRACKET_EXPECTED    "Expected ) bracket at position %ld, but got %s"
#define S_LBRACKET_EXPECTED   "Expected ( bracket at position %ld, but got %s"
#define S_COMMA_EXPECTED      "Expected comma (,) at position %ld, but got %s"
#define S_INVALID_FLOAT       "%s is not a valid floating-point value"
#define S_UNKNOWN_IDENTIFIER  "Unknown identifier: %s"
#define S_IN_EXPRESSION_EMPTY "Cannot evaluate: empty expression"
#define S_INVALID_NUMBER_CHAR "Unexpected character in number : %c"
#define S_INVALID_NUMBER      "Invalid numerical value : %s"
#define S_UNTERM_IDENTIFIER   "Unterminated quoted identifier: %s"
#define S_NO_NEGATION         "Cannot negate expression of type %s : %s"
#define S_NO_NOT_OPERATION    "Cannot perform \"not\" on expression of type %s: %s"
#define S_TYPES_DO_NOT_MATCH  "Type mismatch: %s<>%s for expressions \"%s\" and \"%s\"."
#define S_INVALID_NODE_TYPE   "Node type (%s) not in allowed types (%s) for expression: %s"
#define S_UNTERM_EXPRESSION   "Badly terminated expression. Found token at position %ld : %s"
#define S_INVALID_ARG_COUNT   "Invalid argument count for function %s"
#define S_INVALID_ARG_TYPE    "Invalid type for argument %d: Expected %s, got %s"
#define S_IF_NEEDS_BOOLEAN    "First argument to IF must be of type boolean: %s"
#define S_CASE_NEEDS_3        "Case statement needs to have at least 4 arguments"
#define S_CASE_EVEN_COUNT     "Case statement needs to have an even number of arguments"
#define S_CASE_LABEL_NOT_CONST "Case label %d \"%s\" is not a constant expression"
#define S_CASE_LABEL_TYPE     "Case label %d \"%s\" needs type %s, but has type %s"
#define S_CASE_VALUE_TYPE     "Case value %d \"%s\" needs type %s, but has type %s"
#define S_DIVISION_BY_ZERO    "%s division by zero"

/* RTL exception texts, reached through the FPU and through Pascal's own
   integer `mod`. Live-probed against reference DRF 0.40. */
#define S_FPU_INVALID         "Invalid floating point operation"
#define S_FPU_ZERO_DIVIDE     "Floating point division by zero"
#define S_FPU_OVERFLOW        "Floating point overflow"
#define S_INT_ZERO_DIVIDE     "Division by zero"

/* ------------------------------------------------------------------
   Scanner tokens - TTokenType (fpexprpars.pp:30-35).
   ------------------------------------------------------------------ */
typedef enum {
    TT_EOF = 0,
    TT_PLUS, TT_MINUS, TT_LESSTHAN, TT_LARGERTHAN, TT_EQUAL,
    TT_DIV, TT_MUL, TT_LEFT, TT_RIGHT, TT_COMMA, TT_POWER,
    TT_LESSTHANEQUAL, TT_LARGERTHANEQUAL, TT_UNEQUAL,
    TT_NUMBER, TT_STRING, TT_IDENTIFIER,
    TT_OR, TT_XOR, TT_AND, TT_TRUE, TT_FALSE, TT_NOT, TT_IF, TT_CASE,
    TT_MOD
} TokType;

/* ------------------------------------------------------------------
   Identifiers. The [bcMath] filter admits exactly these fourteen
   (RegisterStdBuiltins, fpexprpars.pp:4380-4397), in this registration
   order; every function takes one 'F' parameter.
   ------------------------------------------------------------------ */
enum {
    FN_NONE = 0,
    FN_COS, FN_SIN, FN_ARCTAN, FN_ABS, FN_SQR, FN_SQRT, FN_EXP,
    FN_LN, FN_LOG, FN_FRAC, FN_INT, FN_ROUND, FN_TRUNC
};

typedef struct {
    const char *name;    /* as registered - FID.Name, what AsString shows */
    int         is_func;
    ResType     rtype;   /* the identifier's result type */
    int         fn;      /* FN_* for is_func, FN_NONE for a variable */
    double      fval;    /* variable payload (rtFloat, always) */
} Ident;

static const Ident BUILTINS[] = {
    /* AddFloatVariable(bcMath,'pi',Pi) - fpexprpars.pp:4382 */
    { "pi",     0, RT_FLOAT,   FN_NONE,   3.14159265358979323846 },
    { "cos",    1, RT_FLOAT,   FN_COS,    0.0 },
    { "sin",    1, RT_FLOAT,   FN_SIN,    0.0 },
    { "arctan", 1, RT_FLOAT,   FN_ARCTAN, 0.0 },
    { "abs",    1, RT_FLOAT,   FN_ABS,    0.0 },
    { "sqr",    1, RT_FLOAT,   FN_SQR,    0.0 },
    { "sqrt",   1, RT_FLOAT,   FN_SQRT,   0.0 },
    { "exp",    1, RT_FLOAT,   FN_EXP,    0.0 },
    { "ln",     1, RT_FLOAT,   FN_LN,     0.0 },
    { "log",    1, RT_FLOAT,   FN_LOG,    0.0 },
    { "frac",   1, RT_FLOAT,   FN_FRAC,   0.0 },
    { "int",    1, RT_FLOAT,   FN_INT,    0.0 },
    { "round",  1, RT_INTEGER, FN_ROUND,  0.0 },
    { "trunc",  1, RT_INTEGER, FN_TRUNC,  0.0 }
};

#define N_BUILTINS ((int)(sizeof BUILTINS / sizeof BUILTINS[0]))

/* ------------------------------------------------------------------
   Parse tree.
   ------------------------------------------------------------------ */
typedef enum {
    N_CONST, N_VAR, N_FUNC, N_IF, N_CASE,
    N_NOT, N_NEGATE, N_INT2FLOAT,
    N_AND, N_OR, N_XOR,
    N_EQUAL, N_UNEQUAL, N_LESSTHAN, N_LESSEQ, N_GREATER, N_GREATEREQ,
    N_ADD, N_SUB, N_MUL, N_DIV, N_MOD, N_POWER
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind      kind;
    Val           cval;   /* N_CONST */
    const Ident  *id;     /* N_VAR, N_FUNC */
    Node         *left;   /* binary left; unary operand; convert operand */
    Node         *right;  /* binary right */
    Node         *cond;   /* N_IF */
    Node        **args;   /* N_FUNC, N_CASE */
    int           nargs;
};

/* ------------------------------------------------------------------
   Parser context. A raise is a longjmp, mirroring the Pascal exception
   the caller's inner TRY..EXCEPT catches (USintactic.pas:126-134).
   ------------------------------------------------------------------ */
typedef struct {
    const char       *src;
    long              len;
    long              pos;      /* FPos - 1-based, 0 for an empty source */
    TokType           tt;
    char             *tok;      /* FToken, NUL terminated */
    long              toklen;
    const SymbolList *syms;
    Arena            *a;
    jmp_buf           jb;
    char              msg[1024];  /* must match ExprResult.msg */
} Ctx;

_Static_assert(sizeof ((Ctx *)0)->msg == sizeof ((ExprResult *)0)->msg,
               "Ctx.msg and ExprResult.msg must be the same size");

static void expr_raise(Ctx *c, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(c->msg, sizeof c->msg, fmt, ap);
    va_end(ap);
    longjmp(c->jb, 1);
}

/* ------------------------------------------------------------------
   Arena string builder, for the AsString renderings the type-check
   texts embed.
   ------------------------------------------------------------------ */
typedef struct {
    Arena  *a;
    char   *p;
    size_t  len;
    size_t  cap;
} SB;

static void sb_init(SB *sb, Arena *a)
{
    sb->a = a;
    sb->cap = 64;
    sb->len = 0;
    sb->p = (char *)arena_alloc(a, sb->cap);
    sb->p[0] = '\0';
}

static void sb_addn(SB *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap;
        char *np;

        while (sb->len + n + 1 > ncap) ncap *= 2;
        np = (char *)arena_alloc(sb->a, ncap);
        memcpy(np, sb->p, sb->len);
        arena_poison(sb->a, sb->p, sb->cap);
        sb->p = np;
        sb->cap = ncap;
    }
    memcpy(sb->p + sb->len, s, n);
    sb->len += n;
    sb->p[sb->len] = '\0';
}

static void sb_add(SB *sb, const char *s) { sb_addn(sb, s, strlen(s)); }

static size_t str_append(char *out, size_t outsz, size_t pos, const char *s)
{
    while (*s != '\0' && pos + 1 < outsz) out[pos++] = *s++;
    if (outsz != 0) out[pos] = '\0';
    return pos;
}

/* FPC Str for Double (fpexprpars.pp:2875, used by AsString for an
   rtFloat constant): sign column (space when positive), 16 fraction
   digits, THREE exponent digits - ' 1.5000000000000000E+000'. The
   sign column is P52's second space. Live-probed. */
static void fpc_float_str(double v, char *out, size_t outsz)
{
    char tmp[64];
    char *e;
    char echar[2];
    const char *edig;
    size_t ndig;
    size_t pos = 0;

    if (outsz == 0) return;
    out[0] = '\0';
    snprintf(tmp, sizeof tmp, "%.16E", v);
    e = strchr(tmp, 'E');
    if (e == NULL) {
        /* Not reachable for a finite double, which is all a const node
           can hold: every literal comes back from Val, and a Val
           overflow raises before a node is built. */
        pos = str_append(out, outsz, pos, " ");
        str_append(out, outsz, pos, tmp);
        return;
    }
    *e = '\0';
    echar[0] = e[1];
    echar[1] = '\0';
    edig = e + 2;
    ndig = strlen(edig);

    if (tmp[0] != '-') pos = str_append(out, outsz, pos, " ");
    pos = str_append(out, outsz, pos, tmp);
    pos = str_append(out, outsz, pos, "E");
    pos = str_append(out, outsz, pos, echar);
    /* FPC's Str always writes three exponent digits. */
    while (ndig < 3) {
        pos = str_append(out, outsz, pos, "0");
        ndig++;
    }
    str_append(out, outsz, pos, edig);
}

/* IntToStr for an Int64 constant (fpexprpars.pp:2872). */
static void fpc_int_str(int64_t v, char *out, size_t outsz)
{
    snprintf(out, outsz, "%lld", (long long)v);
}

/* ------------------------------------------------------------------
   Pascal Val.
   ------------------------------------------------------------------ */

/* Val for a real (fpexprpars.pp:1370, and again at 1975). Returns 0 on
   success, 1 for a malformed value, 2 for the exponent overflow the RTL
   reports as a Floating point overflow ("1e1000", live-probed). */
static int val_real(const char *s, double *out)
{
    char *end;
    double d;

    if (*s == '\0') return 1;
    errno = 0;
    d = strtod(s, &end);
    if (end == s || *end != '\0') return 1;
    if (errno == ERANGE && (d >= HUGE_VAL || d <= -HUGE_VAL)) return 2;
    *out = d;
    return 0;
}

/* TryStrToInt64 (fpexprpars.pp:1971) and the Int64 arm of the Val
   wrapper (1365). Accepts plain decimal digits and the Pascal radix
   prefixes $ (hex), & (octal) and % (binary); the scanner never hands
   it a sign. Returns 1 on success. */
static int try_str_to_int64(const char *s, int64_t *out)
{
    unsigned base = 10;
    uint64_t acc = 0;
    uint64_t limit;
    const char *p = s;

    if (*p == '$') { base = 16; p++; }
    else if (*p == '&') { base = 8; p++; }
    else if (*p == '%') { base = 2; p++; }
    if (*p == '\0') return 0;

    /* An unprefixed literal is a signed decimal, so it stops at
       MaxInt64; a radix literal is read into the same Int64 through the
       unsigned pattern, so $FFFFFFFFFFFFFFFF is -1. */
    limit = (base == 10) ? (uint64_t)INT64_MAX : UINT64_MAX;

    for (; *p != '\0'; p++) {
        unsigned d;
        unsigned char ch = (unsigned char)*p;

        if (ch >= '0' && ch <= '9') d = (unsigned)(ch - '0');
        else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A' + 10);
        else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10);
        else return 0;
        if (d >= base) return 0;
        if (acc > (limit - d) / base) return 0;
        acc = acc * base + d;
    }
    *out = (int64_t)acc;
    return 1;
}

/* ------------------------------------------------------------------
   Scanner character classes - fpexprpars.pp:845-853.
   ------------------------------------------------------------------ */
static int is_white(unsigned char ch)
{
    return ch == ' ' || ch == 13 || ch == 10 || ch == 9;
}

static int is_operator(unsigned char ch)
{
    return ch == '+' || ch == '-' || ch == '<' || ch == '>' ||
           ch == '=' || ch == '/' || ch == '*' || ch == '^';
}

/* Delimiters = Operators+[',','(',')'] - note '%' is NOT one. */
static int is_delim(unsigned char ch)
{
    return is_operator(ch) || ch == ',' || ch == '(' || ch == ')';
}

/* WordDelimiters = WhiteSpace + Symbols, Symbols = ['%']+Delimiters. */
static int is_word_delim(unsigned char ch)
{
    return is_white(ch) || ch == '%' || is_delim(ch);
}

static int is_alpha(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

typedef enum { NK_DECIMAL, NK_HEX, NK_OCTAL, NK_BINARY } NumKind;

static int is_num_digit(unsigned char ch, NumKind k)
{
    switch (k) {
    case NK_DECIMAL: return (ch >= '0' && ch <= '9') || ch == '.';
    case NK_HEX:     return (ch >= '0' && ch <= '9') ||
                            (ch >= 'A' && ch <= 'F') ||
                            (ch >= 'a' && ch <= 'f');
    case NK_OCTAL:   return ch >= '0' && ch <= '7';
    case NK_BINARY:  return ch == '0' || ch == '1';
    }
    return 0;
}

static char up_case(char ch)
{
    return (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
}

/* ASCII LowerCase, which is what both the keyword match
   (fpexprpars.pp:1448) and the identifier hash (1526-1548) use. */
static char low_case(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int same_ident(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (low_case(*a) != low_case(*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ------------------------------------------------------------------
   Scanner. FPos and FChar move in lockstep in the Pascal
   (fpexprpars.pp:1236-1241), so one 1-based index does for both; the
   character one past the end is the AnsiString's own terminating NUL,
   which every scan loop stops on.
   ------------------------------------------------------------------ */
static unsigned char cur_char(const Ctx *c)
{
    if (c->pos < 1 || c->pos > c->len) return 0;
    return (unsigned char)c->src[c->pos - 1];
}

static unsigned char next_pos(Ctx *c)
{
    c->pos++;
    return cur_char(c);
}

static void tok_clear(Ctx *c) { c->toklen = 0; c->tok[0] = '\0'; }

static void tok_add(Ctx *c, unsigned char ch)
{
    c->tok[c->toklen++] = (char)ch;
    c->tok[c->toklen] = '\0';
}

static void skip_white(Ctx *c)
{
    while (is_white(cur_char(c)) && c->pos <= c->len) next_pos(c);
}

static TokType do_delimiter(Ctx *c)
{
    unsigned char ch = cur_char(c);
    unsigned char d = ch;
    int b = (ch == '<' || ch == '>');

    tok_add(c, ch);
    ch = next_pos(c);

    if (b && (ch == '=' || ch == '>')) {
        tok_add(c, ch);
        next_pos(c);
        if (d == '>') return TT_LARGERTHANEQUAL;
        if (ch == '>') return TT_UNEQUAL;
        return TT_LESSTHANEQUAL;
    }
    switch (d) {
    case '+': return TT_PLUS;
    case '-': return TT_MINUS;
    case '<': return TT_LESSTHAN;
    case '>': return TT_LARGERTHAN;
    case '=': return TT_EQUAL;
    case '/': return TT_DIV;
    case '*': return TT_MUL;
    case '(': return TT_LEFT;
    case ')': return TT_RIGHT;
    case ',': return TT_COMMA;
    case '^': return TT_POWER;
    default:  break;
    }
    expr_raise(c, S_UNKNOWN_DELIMITER, (char)d);
    return TT_EOF;
}

/* DoString - fpexprpars.pp:1320-1349. '' is an escaped quote. */
static TokType do_string(Ctx *c)
{
    unsigned char ch;
    int terminating;

    tok_clear(c);
    ch = next_pos(c);
    for (;;) {
        terminating = (ch == 0) ||
                      (ch == '\'' &&
                       !(c->pos < c->len &&
                         (unsigned char)c->src[c->pos] == '\''));
        if (terminating) break;
        tok_add(c, ch);
        if (ch == '\'') next_pos(c);
        ch = next_pos(c);
    }
    if (ch == 0) expr_raise(c, S_BAD_QUOTES);
    next_pos(c);
    return TT_STRING;
}

/* DoNumber - fpexprpars.pp:1373-1422, transcribed including the
   prevC state machine that lets an exponent sign through and lets '%',
   '$' and '&' lead. */
static TokType do_number(Ctx *c, NumKind kind)
{
    unsigned char ch = cur_char(c);
    char prev = '\0';

    while (ch != 0) {
        int valid;

        if (is_word_delim(ch)) {
            if (kind == NK_DECIMAL) {
                if (!(prev == 'E' || prev == '-' || prev == '+')) break;
            } else if (kind == NK_HEX || kind == NK_OCTAL) {
                break;
            } else { /* NK_BINARY - allow '%' as first char */
                if (prev != '\0') break;
            }
        }
        valid = is_num_digit(ch, kind);
        if (!valid) {
            switch (kind) {
            case NK_DECIMAL:
                valid = (c->toklen != 0 && up_case((char)ch) == 'E') ||
                        (c->toklen != 0 && (ch == '+' || ch == '-') &&
                         prev == 'E');
                break;
            case NK_HEX:    valid = (ch == '$') && (prev == '\0'); break;
            case NK_OCTAL:  valid = (ch == '&') && (prev == '\0'); break;
            case NK_BINARY: valid = (ch == '%') && (prev == '\0'); break;
            }
        }
        if (!valid) expr_raise(c, S_INVALID_NUMBER_CHAR, (char)ch);
        tok_add(c, ch);
        prev = up_case((char)ch);
        ch = next_pos(c);
    }

    /* The Val wrapper at fpexprpars.pp:1359-1371: a radix prefix goes
       through the Int64 reader, everything else through the real one.
       Here it is purely a validity check - Primitive re-reads the token
       to build the node. */
    if (c->tok[0] == '$' || c->tok[0] == '&' || c->tok[0] == '%') {
        int64_t iv;
        if (!try_str_to_int64(c->tok, &iv))
            expr_raise(c, S_INVALID_NUMBER, c->tok);
    } else {
        double dv;
        int code = val_real(c->tok, &dv);
        if (code == 2) expr_raise(c, S_FPU_OVERFLOW);
        if (code != 0) expr_raise(c, S_INVALID_NUMBER, c->tok);
    }
    return TT_NUMBER;
}

/* DoIdentifier - fpexprpars.pp:1424-1469. Continues on ANY character
   that is not a word delimiter, so digits, '_', '.', '#' and '?' are
   all legal inside a name; the case-insensitive keyword match is the
   tail of the same routine. */
static TokType do_identifier(Ctx *c)
{
    unsigned char ch = cur_char(c);
    size_t i;
    char lowered[32];

    while (!is_word_delim(ch) && ch != 0) {
        if (ch != '"') {
            tok_add(c, ch);
        } else {
            ch = next_pos(c);
            while (ch != 0 && ch != '"') {
                tok_add(c, ch);
                ch = next_pos(c);
            }
            if (ch != '"') expr_raise(c, S_UNTERM_IDENTIFIER, c->tok);
        }
        ch = next_pos(c);
    }

    if ((size_t)c->toklen < sizeof lowered) {
        for (i = 0; i < (size_t)c->toklen; i++) lowered[i] = low_case(c->tok[i]);
        lowered[i] = '\0';
        if (strcmp(lowered, "or") == 0)    return TT_OR;
        if (strcmp(lowered, "xor") == 0)   return TT_XOR;
        if (strcmp(lowered, "and") == 0)   return TT_AND;
        if (strcmp(lowered, "true") == 0)  return TT_TRUE;
        if (strcmp(lowered, "false") == 0) return TT_FALSE;
        if (strcmp(lowered, "not") == 0)   return TT_NOT;
        if (strcmp(lowered, "if") == 0)    return TT_IF;
        if (strcmp(lowered, "case") == 0)  return TT_CASE;
        if (strcmp(lowered, "mod") == 0)   return TT_MOD;
    }
    return TT_IDENTIFIER;
}

/* GetToken - fpexprpars.pp:1471-1499. */
static void get_token(Ctx *c)
{
    unsigned char ch;

    tok_clear(c);
    skip_white(c);
    ch = cur_char(c);
    if (ch == 0)                 c->tt = TT_EOF;
    else if (is_delim(ch))       c->tt = do_delimiter(c);
    else if (ch == '\'')         c->tt = do_string(c);
    else if (ch == '$')          c->tt = do_number(c, NK_HEX);
    else if (ch == '&')          c->tt = do_number(c, NK_OCTAL);
    else if (ch == '%')          c->tt = do_number(c, NK_BINARY);
    else if (is_num_digit(ch, NK_DECIMAL))
                                 c->tt = do_number(c, NK_DECIMAL);
    else if (is_alpha(ch) || ch == '"')
                                 c->tt = do_identifier(c);
    else
        expr_raise(c, S_UNKNOWN_CHARACTER, c->pos, (char)ch);
}

static void check_eof(Ctx *c)
{
    if (c->tt == TT_EOF) expr_raise(c, S_UNEXPECTED_EOE);
}

/* ------------------------------------------------------------------
   Identifier lookup. Builtins go into the hash first and user
   identifiers after (fpexprpars.pp:1526-1540), and the probe pins the
   USER entry as the one Find returns - so a #define shadows a bcMath
   name for every later expression (19.58).
   ------------------------------------------------------------------ */
static const Ident *identifier_by_name(Ctx *c, const char *name)
{
    size_t n, i;

    if (c->syms != NULL) {
        n = symbols_count(c->syms);
        for (i = 0; i < n; i++) {
            const char *sname = NULL;
            long sval = 0;

            if (!symbols_at(c->syms, i, &sname, &sval)) continue;
            if (same_ident(sname, name)) {
                Ident *id = (Ident *)arena_alloc(c->a, sizeof *id);
                id->name = sname;   /* the FOLDED stored name - FID.Name */
                id->is_func = 0;
                id->rtype = RT_FLOAT;
                id->fn = FN_NONE;
                /* AddFloatVariable widens Longint to Double; a Double
                   holds every 32-bit integer exactly. */
                id->fval = (double)sval;
                return id;
            }
        }
    }
    for (i = 0; i < (size_t)N_BUILTINS; i++)
        if (same_ident(BUILTINS[i].name, name)) return &BUILTINS[i];
    return NULL;
}

/* ------------------------------------------------------------------
   Node constructors and node type.
   ------------------------------------------------------------------ */
static Node *node_new(Ctx *c, NodeKind kind)
{
    Node *n = (Node *)arena_calloc(c->a, sizeof *n);
    n->kind = kind;
    return n;
}

static Node *node_bin(Ctx *c, NodeKind kind, Node *l, Node *r)
{
    Node *n = node_new(c, kind);
    n->left = l;
    n->right = r;
    return n;
}

static ResType node_type(const Node *n)
{
    switch (n->kind) {
    case N_CONST:      return n->cval.type;
    case N_VAR:
    case N_FUNC:       return n->id->rtype;
    case N_IF:         return node_type(n->left);
    /* TCaseOperation.NodeType is FArgs[1].NodeType (fpexprpars.pp:3235);
       a case() with fewer than two arguments reads off the end of the
       dynamic array in the reference, which is only reachable when the
       node is NOT the root (Check would have rejected it) - guarded
       here rather than reproducing an out-of-bounds read. */
    case N_CASE:       return (n->nargs > 1) ? node_type(n->args[1])
                                             : RT_INTEGER;
    case N_NOT:
    case N_NEGATE:     return node_type(n->left);
    case N_INT2FLOAT:  return RT_FLOAT;
    case N_AND:
    case N_OR:
    case N_XOR:        return node_type(n->left);
    case N_EQUAL:
    case N_UNEQUAL:
    case N_LESSTHAN:
    case N_LESSEQ:
    case N_GREATER:
    case N_GREATEREQ:  return RT_BOOLEAN;
    case N_ADD:
    case N_SUB:
    case N_MUL:        return node_type(n->left);
    case N_DIV:        return RT_FLOAT;
    case N_MOD:        return RT_INTEGER;
    case N_POWER:      return RT_FLOAT;
    }
    return RT_INTEGER;
}

/* AsString, the renderings the type-check texts embed. A convert node
   delegates to its operand (fpexprpars.pp:3644), which is why an
   int-to-float promoted operand still prints as the integer it was
   written as. */
static void as_string(const Node *n, SB *sb)
{
    char buf[64];
    int i;

    switch (n->kind) {
    case N_CONST:
        switch (n->cval.type) {
        case RT_STRING:
            sb_add(sb, "'"); sb_add(sb, n->cval.s); sb_add(sb, "'");
            break;
        case RT_INTEGER:
            fpc_int_str(n->cval.i, buf, sizeof buf); sb_add(sb, buf);
            break;
        case RT_BOOLEAN:
            sb_add(sb, n->cval.b ? "True" : "False");
            break;
        default:
            fpc_float_str(n->cval.f, buf, sizeof buf); sb_add(sb, buf);
            break;
        }
        return;
    case N_VAR:
    case N_FUNC:
        sb_add(sb, n->id->name);
        if (n->kind == N_FUNC && n->nargs > 0) {
            sb_add(sb, "(");
            for (i = 0; i < n->nargs; i++) {
                if (i > 0) sb_add(sb, ",");
                as_string(n->args[i], sb);
            }
            sb_add(sb, ")");
        }
        return;
    case N_IF:
        sb_add(sb, "if(");
        as_string(n->cond, sb);  sb_add(sb, " , ");
        as_string(n->left, sb);  sb_add(sb, " , ");
        as_string(n->right, sb); sb_add(sb, ")");
        return;
    case N_CASE:
        sb_add(sb, "Case(");
        for (i = 0; i < n->nargs; i++) {
            if (i > 0) sb_add(sb, ", ");
            as_string(n->args[i], sb);
        }
        sb_add(sb, ")");
        return;
    case N_NOT:
        sb_add(sb, "not ");
        as_string(n->left, sb);
        return;
    case N_NEGATE: {
        /* '-'+TrimLeft(Operand.AsString) - fpexprpars.pp:2907, which is
           what eats Str's sign column on a negated float constant. */
        SB inner;
        const char *p;

        sb_init(&inner, sb->a);
        as_string(n->left, &inner);
        p = inner.p;
        while (*p == ' ' || *p == '\t') p++;
        sb_add(sb, "-");
        sb_add(sb, p);
        return;
    }
    case N_INT2FLOAT:
        as_string(n->left, sb);
        return;
    default:
        break;
    }
    as_string(n->left, sb);
    switch (n->kind) {
    case N_AND:        sb_add(sb, " and "); break;
    case N_OR:         sb_add(sb, " or ");  break;
    case N_XOR:        sb_add(sb, " xor "); break;
    case N_EQUAL:      sb_add(sb, " = ");   break;
    case N_UNEQUAL:    sb_add(sb, " <> ");  break;
    case N_LESSTHAN:   sb_add(sb, " < ");   break;
    case N_LESSEQ:     sb_add(sb, " <= ");  break;
    case N_GREATER:    sb_add(sb, " > ");   break;
    case N_GREATEREQ:  sb_add(sb, " >= ");  break;
    case N_ADD:        sb_add(sb, " + ");   break;
    case N_SUB:        sb_add(sb, " - ");   break;
    case N_MUL:        sb_add(sb, " * ");   break;
    case N_DIV:        sb_add(sb, " / ");   break;
    case N_MOD:        sb_add(sb, " mod "); break;
    case N_POWER:      sb_add(sb, "^");     break;
    default:           break;
    }
    as_string(n->right, sb);
}

static const char *render(Ctx *c, const Node *n)
{
    SB sb;

    sb_init(&sb, c->a);
    as_string(n, &sb);
    return sb.p;
}

/* ------------------------------------------------------------------
   Type checks - fpexprpars.pp:2946-2967 and the per-node Check methods.
   ------------------------------------------------------------------ */
static void check_node_type(Ctx *c, const Node *n, unsigned allowed)
{
    ResType nt = node_type(n);
    char list[128];
    size_t used = 0;
    int t;

    if (allowed & TSET(nt)) return;
    list[0] = '\0';
    for (t = 0; t < RT_COUNT; t++) {
        size_t len;

        if (!(allowed & TSET((ResType)t))) continue;
        if (used != 0 && used + 1 < sizeof list) list[used++] = ',';
        len = strlen(RT_NAMES[t]);
        if (used + len + 1 > sizeof list) len = sizeof list - used - 1;
        memcpy(list + used, RT_NAMES[t], len);
        used += len;
        list[used] = '\0';
    }
    expr_raise(c, S_INVALID_NODE_TYPE, RT_NAMES[nt], list, render(c, n));
}

static void check_same_node_types(Ctx *c, const Node *n)
{
    ResType lt = node_type(n->left);
    ResType rt = node_type(n->right);
    const char *ls;

    if (lt == rt) return;
    ls = render(c, n->left);
    expr_raise(c, S_TYPES_DO_NOT_MATCH, RT_NAMES[lt], RT_NAMES[rt], ls,
               render(c, n->right));
}

static ResType char_to_result_type(char ch)
{
    switch (up_case(ch)) {
    case 'I': return RT_INTEGER;
    case 'B': return RT_BOOLEAN;
    case 'S': return RT_STRING;
    case 'D': return RT_DATETIME;
    case 'C': return RT_CURRENCY;
    default:  return RT_FLOAT;
    }
}

static Node *convert_node(Ctx *c, Node *n, ResType to);

/* Check, as SetExpression calls it: on the ROOT NODE ONLY
   (fpexprpars.pp:2075). No Check method descends into its children, so
   a type error under a well-typed root is simply not diagnosed -
   "(1.5 and 1)+0" compiles to 1 in the reference. */
static void check_root(Ctx *c, Node *n)
{
    int i;

    switch (n->kind) {
    case N_CONST:      /* fpexprpars.pp:2853 - nothing to check */
    case N_VAR:        /* fpexprpars.pp:3769 - do nothing */
    case N_INT2FLOAT:  /* not reachable as a root */
        return;

    case N_FUNC:
        /* TFPExprFunction.Check, fpexprpars.pp:3820-3839. Every bcMath
           function takes exactly one 'F' argument. */
        if (n->nargs != 1)
            expr_raise(c, S_INVALID_ARG_COUNT, n->id->name);
        for (i = 0; i < n->nargs; i++) {
            ResType rtp = char_to_result_type('F');
            ResType rta = node_type(n->args[i]);
            Node *conv;

            if (rtp == rta) continue;
            conv = convert_node(c, n->args[i], rtp);
            if (conv == n->args[i])
                expr_raise(c, S_INVALID_ARG_TYPE, i + 1, RT_NAMES[rtp],
                           RT_NAMES[rta]);
            n->args[i] = conv;
        }
        return;

    case N_IF:
        /* fpexprpars.pp:3086-3092 */
        if (node_type(n->cond) != RT_BOOLEAN)
            expr_raise(c, S_IF_NEEDS_BOOLEAN, render(c, n->cond));
        check_same_node_types(c, n);
        return;

    case N_CASE:
        /* fpexprpars.pp:3161-3192 */
        if (n->nargs < 3) expr_raise(c, S_CASE_NEEDS_3);
        if ((n->nargs % 2) == 1) expr_raise(c, S_CASE_EVEN_COUNT);
        for (i = 2; i < n->nargs; i++) {
            Node *arg = n->args[i];

            if ((i % 2) == 0) {
                if (arg->kind != N_CONST)
                    expr_raise(c, S_CASE_LABEL_NOT_CONST, i / 2,
                               render(c, arg));
                if (node_type(arg) != node_type(n->args[0]))
                    expr_raise(c, S_CASE_LABEL_TYPE, i / 2, render(c, arg),
                               RT_NAMES[node_type(n->args[0])],
                               RT_NAMES[node_type(arg)]);
            } else if (node_type(arg) != node_type(n->args[1])) {
                expr_raise(c, S_CASE_VALUE_TYPE, (i - 1) / 2,
                           render(c, arg),
                           RT_NAMES[node_type(n->args[1])],
                           RT_NAMES[node_type(arg)]);
            }
        }
        return;

    case N_NOT:
        /* fpexprpars.pp:3037-3041 */
        if (!(SET_BOOL_INT & TSET(node_type(n->left))))
            expr_raise(c, S_NO_NOT_OPERATION, RT_NAMES[node_type(n->left)],
                       render(c, n->left));
        return;

    case N_NEGATE:
        /* fpexprpars.pp:2883-2888 */
        if (!(SET_NEGATABLE & TSET(node_type(n->left))))
            expr_raise(c, S_NO_NEGATION, RT_NAMES[node_type(n->left)],
                       render(c, n->left));
        return;

    case N_AND:
    case N_OR:
    case N_XOR:
        /* TFPBooleanOperation.Check, fpexprpars.pp:2912-2918 */
        check_node_type(c, n->left, SET_BOOL_INT);
        check_node_type(c, n->right, SET_BOOL_INT);
        check_same_node_types(c, n);
        return;

    case N_LESSTHAN:
    case N_LESSEQ:
    case N_GREATER:
    case N_GREATEREQ:
        /* TFPOrderingOperation.Check, fpexprpars.pp:3412-3421, then
           TFPBooleanResultOperation.Check (3272-3276) */
        check_node_type(c, n->left, SET_MATH);
        check_node_type(c, n->right, SET_MATH);
        check_same_node_types(c, n);
        return;

    case N_EQUAL:
    case N_UNEQUAL:
        /* TFPBooleanResultOperation.Check, fpexprpars.pp:3272-3276 */
        check_same_node_types(c, n);
        return;

    case N_ADD:
        /* TMathOperation.Check, fpexprpars.pp:3425-3435 */
        check_node_type(c, n->left, SET_MATH);
        check_node_type(c, n->right, SET_MATH);
        check_same_node_types(c, n);
        return;

    case N_SUB:
        /* fpexprpars.pp:3469-3478, then TMathOperation.Check */
        check_node_type(c, n->left, SET_SUB);
        check_node_type(c, n->right, SET_SUB);
        check_node_type(c, n->left, SET_MATH);
        check_node_type(c, n->right, SET_MATH);
        check_same_node_types(c, n);
        return;

    case N_MUL:
    case N_DIV:
        /* fpexprpars.pp:3503-3512 and 3535-3543, then TMathOperation */
        check_node_type(c, n->left, SET_NUM);
        check_node_type(c, n->right, SET_NUM);
        check_node_type(c, n->left, SET_MATH);
        check_node_type(c, n->right, SET_MATH);
        check_same_node_types(c, n);
        return;

    case N_MOD:
        /* fpexprpars.pp:993-998, then TMathOperation.Check */
        check_node_type(c, n->left, SET_INT);
        check_node_type(c, n->right, SET_INT);
        check_node_type(c, n->left, SET_MATH);
        check_node_type(c, n->right, SET_MATH);
        check_same_node_types(c, n);
        return;

    case N_POWER:
        /* fpexprpars.pp:3595-3601 - no inherited Check, so no
           same-type check here */
        check_node_type(c, n->left, SET_NUM);
        check_node_type(c, n->right, SET_NUM);
        return;
    }
}

/* ConvertNode (fpexprpars.pp:1620-1640), narrowed to the one conversion
   this configuration can produce: integer to float. Returns the node
   unchanged when no conversion applies, which is how ConvertArgument
   detects a bad argument type. */
static Node *convert_node(Ctx *c, Node *n, ResType to)
{
    if (node_type(n) == RT_INTEGER && to == RT_FLOAT) {
        Node *conv = node_new(c, N_INT2FLOAT);
        conv->left = n;
        return conv;
    }
    return n;
}

/* MatchNodes / CheckNodes - fpexprpars.pp:1721-1753. */
static void check_nodes(Ctx *c, Node **left, Node **right)
{
    ResType lt = node_type(*left);
    ResType rt = node_type(*right);

    if (lt != rt) {
        if (lt == RT_INTEGER && rt == RT_FLOAT)
            *left = convert_node(c, *left, RT_FLOAT);
        else if (rt == RT_INTEGER && lt == RT_FLOAT)
            *right = convert_node(c, *right, RT_FLOAT);
    }
}

/* ------------------------------------------------------------------
   Recursive descent - fpexprpars.pp:1762-2059.
   ------------------------------------------------------------------ */
static Node *level1(Ctx *c);
static Node *level2(Ctx *c);
static Node *level3(Ctx *c);
static Node *level4(Ctx *c);
static Node *level5(Ctx *c);
static Node *level6(Ctx *c);
static Node *level7(Ctx *c);
static Node *primitive(Ctx *c);

static Node *level1(Ctx *c)
{
    Node *result;
    Node *right;

    if (c->tt == TT_NOT) {
        get_token(c);
        check_eof(c);
        right = level2(c);
        result = node_new(c, N_NOT);
        result->left = right;
    } else {
        result = level2(c);
    }
    /* ONE flat level: `and` does NOT bind tighter than `or`. */
    while (c->tt == TT_AND || c->tt == TT_OR || c->tt == TT_XOR) {
        TokType tt = c->tt;

        get_token(c);
        check_eof(c);
        right = level2(c);
        if (tt == TT_OR)       result = node_bin(c, N_OR, result, right);
        else if (tt == TT_AND) result = node_bin(c, N_AND, result, right);
        else                   result = node_bin(c, N_XOR, result, right);
    }
    return result;
}

static Node *level2(Ctx *c)
{
    Node *result;
    Node *right;
    TokType tt;
    NodeKind kind;

    result = level3(c);
    tt = c->tt;
    /* An `if`, not a loop (fpexprpars.pp:1811): comparisons do not
       chain. */
    if (tt == TT_LESSTHAN || tt == TT_LESSTHANEQUAL ||
        tt == TT_LARGERTHAN || tt == TT_LARGERTHANEQUAL ||
        tt == TT_EQUAL || tt == TT_UNEQUAL) {
        get_token(c);
        check_eof(c);
        right = level3(c);
        check_nodes(c, &result, &right);
        switch (tt) {
        case TT_LESSTHAN:          kind = N_LESSTHAN;   break;
        case TT_LESSTHANEQUAL:     kind = N_LESSEQ;     break;
        case TT_LARGERTHAN:        kind = N_GREATER;    break;
        case TT_LARGERTHANEQUAL:   kind = N_GREATEREQ;  break;
        case TT_EQUAL:             kind = N_EQUAL;      break;
        default:                   kind = N_UNEQUAL;    break;
        }
        result = node_bin(c, kind, result, right);
    }
    return result;
}

static Node *level3(Ctx *c)
{
    Node *result = level4(c);
    Node *right;

    while (c->tt == TT_PLUS || c->tt == TT_MINUS) {
        TokType tt = c->tt;

        get_token(c);
        check_eof(c);
        right = level4(c);
        check_nodes(c, &result, &right);
        result = node_bin(c, (tt == TT_PLUS) ? N_ADD : N_SUB, result, right);
    }
    return result;
}

static Node *level4(Ctx *c)
{
    Node *result = level5(c);
    Node *right;

    /* No CheckEOF here, unlike level 3 - fpexprpars.pp:1877-1888. */
    while (c->tt == TT_MUL || c->tt == TT_DIV || c->tt == TT_MOD) {
        TokType tt = c->tt;
        NodeKind kind;

        get_token(c);
        right = level5(c);
        check_nodes(c, &result, &right);
        kind = (tt == TT_MUL) ? N_MUL : (tt == TT_DIV) ? N_DIV : N_MOD;
        result = node_bin(c, kind, result, right);
    }
    return result;
}

static Node *level5(Ctx *c)
{
    int negate = 0;
    Node *result;

    /* At most one unary sign; a second one has to arrive as a fresh
       level 5 under a binary operator, which is why "2--3" is 5. */
    if (c->tt == TT_PLUS || c->tt == TT_MINUS) {
        negate = (c->tt == TT_MINUS);
        get_token(c);
    }
    result = level6(c);
    if (negate) {
        Node *n = node_new(c, N_NEGATE);
        n->left = result;
        result = n;
    }
    return result;
}

static Node *level6(Ctx *c)
{
    Node *result = level7(c);
    Node *right;

    while (c->tt == TT_POWER) {
        get_token(c);
        right = level5(c); /* accepts '(' and unary '+'/'-' - and, via
                              level 5's fall-through to level 6, the
                              whole rest of the power chain */
        check_nodes(c, &result, &right);
        result = node_bin(c, N_POWER, result, right);
    }
    return result;
}

static Node *level7(Ctx *c)
{
    Node *result;

    if (c->tt == TT_LEFT) {
        get_token(c);
        result = level1(c);
        if (c->tt != TT_RIGHT)
            expr_raise(c, S_BRACKET_EXPECTED, c->pos, c->tok);
        get_token(c);
        return result;
    }
    return primitive(c);
}

static Node *primitive(Ctx *c)
{
    Node *result = NULL;
    int64_t iv;
    double dv;

    if (c->tt == TT_NUMBER) {
        result = node_new(c, N_CONST);
        if (try_str_to_int64(c->tok, &iv)) {
            result->cval.type = RT_INTEGER;
            result->cval.i = iv;
        } else {
            int code = val_real(c->tok, &dv);

            if (code == 2) expr_raise(c, S_FPU_OVERFLOW);
            if (code != 0) expr_raise(c, S_INVALID_FLOAT, c->tok);
            result->cval.type = RT_FLOAT;
            result->cval.f = dv;
        }
    } else if (c->tt == TT_STRING) {
        result = node_new(c, N_CONST);
        result->cval.type = RT_STRING;
        result->cval.s = arena_strdup(c->a, c->tok);
    } else if (c->tt == TT_TRUE || c->tt == TT_FALSE) {
        result = node_new(c, N_CONST);
        val_bool(&result->cval, c->tt == TT_TRUE);
    } else if (c->tt != TT_IDENTIFIER && c->tt != TT_IF && c->tt != TT_CASE) {
        expr_raise(c, S_UNKNOWN_TOKEN_AT, c->pos, c->tok);
    } else {
        int is_if = (c->tt == TT_IF);
        int is_case = (c->tt == TT_CASE);
        const Ident *id = NULL;
        int acount;
        int ai = 0;
        int cap;
        Node **args = NULL;

        if (!is_if && !is_case) {
            id = identifier_by_name(c, c->tok);
            if (id == NULL) expr_raise(c, S_UNKNOWN_IDENTIFIER, c->tok);
        }
        if (is_if)             acount = 3;
        else if (is_case)      acount = -4;
        else if (id->is_func)  acount = 1;
        else                   acount = 0;

        if (acount != 0) {
            cap = (acount < 0) ? -acount : acount;
            args = (Node **)arena_calloc(c->a, (size_t)cap * sizeof *args);
            get_token(c);
            if (c->tt != TT_LEFT)
                expr_raise(c, S_LBRACKET_EXPECTED, c->pos, c->tok);
            do {
                get_token(c);
                if (acount < 0 && ai == cap) {
                    Node **grown;
                    int k;

                    grown = (Node **)arena_calloc(c->a,
                                (size_t)(cap * 2) * sizeof *grown);
                    for (k = 0; k < cap; k++) grown[k] = args[k];
                    arena_poison(c->a, args, (size_t)cap * sizeof *args);
                    args = grown;
                    cap *= 2;
                }
                args[ai] = level1(c);
                ai++;
                if (c->tt != TT_COMMA && ai < ((acount < 0) ? -acount : acount))
                    expr_raise(c, S_COMMA_EXPECTED, c->pos, c->tok);
            } while (!(ai == acount || (acount < 0 && c->tt == TT_RIGHT)));
            if (c->tt != TT_RIGHT)
                expr_raise(c, S_BRACKET_EXPECTED, c->pos, c->tok);
        }

        if (is_if) {
            result = node_new(c, N_IF);
            result->cond = args[0];
            result->left = args[1];
            result->right = args[2];
        } else if (is_case) {
            result = node_new(c, N_CASE);
            result->args = args;
            result->nargs = ai;
        } else if (id->is_func) {
            result = node_new(c, N_FUNC);
            result->id = id;
            result->args = args;
            result->nargs = ai;
        } else {
            result = node_new(c, N_VAR);
            result->id = id;
        }
    }
    get_token(c);
    return result;
}

/* ------------------------------------------------------------------
   Evaluation.
   ------------------------------------------------------------------ */

/* Int64 arithmetic with FPC's checks off (fpexprpars.pp:16-17): the
   wrap is the observable behaviour, so do it in uint64 rather than
   invoking C signed-overflow UB. */
static int64_t wrap_add(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a + (uint64_t)b);
}
static int64_t wrap_sub(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a - (uint64_t)b);
}
static int64_t wrap_mul(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a * (uint64_t)b);
}

/* A NaN can only come out of an operation the FPU calls invalid: no
   operand can already be one, because whatever produced it would have
   raised first. An infinity out of finite operands is the overflow the
   RTL reports separately. */
static double fchk(Ctx *c, double r, int operands_finite)
{
    if (isnan(r)) expr_raise(c, S_FPU_INVALID);
    if (operands_finite && isinf(r)) expr_raise(c, S_FPU_OVERFLOW);
    return r;
}

/* FPC's math functions take/return ValReal, the x87's 80-bit type; only
   the STORE into ResFloat rounds to Double (fpexprpars.pp:33). The
   reference's "2^3" is exactly 8 and "3^5" exactly 243 because of that
   extra precision - a Double exp(y*ln(x)) lands 2 ulp low and truncs to
   7. long double is the same 80-bit type on x86 gcc (LDBL_MANT_DIG 64);
   a target whose long double is only a Double may differ in the last
   ulp of a transcendental result. */
#if LDBL_MANT_DIG < 64
#error "NDRC needs an 80-bit long double. Where it is only a Double \
(Apple arm64), an expression's 2^3 compiles to 7 instead of 8."
#endif
static double fstore(Ctx *c, long double r, int operands_finite)
{
    return fchk(c, (double)r, operands_finite);
}

static long double fp_ln(Ctx *c, long double x)
{
    long double r;

    if (x == 0.0L) expr_raise(c, S_FPU_ZERO_DIVIDE);
    r = logl(x);
    if (isnan(r)) expr_raise(c, S_FPU_INVALID);
    return r;
}

/* Round and Trunc return Int64; a double outside that range makes the
   conversion invalid, which the reference reports as an FPU fault -
   live-probed, trunc(1e19) and round(1e19) both give `Invalid floating
   point operation` while trunc(9.2e18) converts and dies later in the
   caller's Longint store instead. */
static void fp_check_int64_range(Ctx *c, double x)
{
    if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
        expr_raise(c, S_FPU_INVALID);
}

static int64_t fp_to_int64(Ctx *c, double x)
{
    fp_check_int64_range(c, x);
    return (int64_t)x;
}

/* power - fpexprpars.pp:3613-3630, transcribed. NOT pow(). */
static double fp_power(Ctx *c, double base, double exponent)
{
    long double e = (long double)exponent;
    double r;

    if (exponent == 0.0) return 1.0;
    if (base == 0.0 && exponent > 0.0) return 0.0;
    if (base < 0.0 && (exponent - trunc(exponent)) == 0.0) {
        /* `ex := round(exponent)` (fpexprpars.pp:3624) runs BEFORE the
           exp/ln on the next line, and Round of a double outside the
           Int64 range raises the same fault trunc(1e19) does - so the
           magnitude test has to come first, or the exp's overflow gets
           reported in its place. Live: "(0-2)^9.3e18" is `Invalid
           floating point operation` while "(0-2)^9.2e18" is `Floating
           point overflow`.

           Inside the Int64 range nothing else is needed: the Pascal
           narrows Round's Int64 into an `ex: Integer` unchecked (the
           unit sets no $R), so the high bits are simply dropped and bit
           0 - the only bit `odd(ex)` looks at - survives. fmod reads
           that same bit. */
        double ex = nearbyint(exponent);

        fp_check_int64_range(c, ex);
        r = fstore(c, expl(e * fp_ln(c, (long double)-base)), 1);
        if (fmod(ex, 2.0) != 0.0) r = -r;
        return r;
    }
    return fstore(c, expl(e * fp_ln(c, (long double)base)), 1);
}

static double arg_to_float(const Val *v)
{
    /* ArgToFloat, fpexprpars.pp:3942-3953 - accepts an integer in place
       of a float, so an unconverted integer argument still works.

       The unit's rtCurrency arm is PORTED-DORMANT and deliberately not
       reproduced: nothing in this configuration can make a currency
       value (no currency literal, no AddCurrencyVariable call, no
       bcMath function returning one), and now that Val is a union,
       spelling it as a plain `return v->f` for rtCurrency would read
       the wrong bytes rather than harmlessly duplicating the float
       arm - a live port of it would have to scale Val.cur by 10000. */
    if (v->type == RT_INTEGER) return (double)v->i;
    return v->f;
}

static void eval(Ctx *c, const Node *n, Val *out);

static void eval_builtin(Ctx *c, const Node *n, Val *out)
{
    Val arg;
    double x;
    long double lx;
    double r = 0.0;

    eval(c, n->args[0], &arg);
    x = arg_to_float(&arg);
    lx = (long double)x;

    switch (n->id->fn) {
    case FN_COS:    r = fstore(c, cosl(lx), 0); break;
    case FN_SIN:    r = fstore(c, sinl(lx), 0); break;
    case FN_ARCTAN: r = fstore(c, atanl(lx), 0); break;
    case FN_ABS:    r = fstore(c, fabsl(lx), 0); break;
    case FN_SQR:    r = fstore(c, lx * lx, isfinite(x)); break;
    case FN_SQRT:   r = fstore(c, sqrtl(lx), 0); break;
    case FN_EXP:    r = fstore(c, expl(lx), isfinite(x)); break;
    case FN_LN:     r = fstore(c, fp_ln(c, lx), 0); break;
    /* Const L10 = ln(10) - a compile-time real constant, so the
       division happens at the same width as Ln's own result
       (fpexprpars.pp:3997-4003). */
    case FN_LOG:    r = fstore(c, fp_ln(c, lx) / logl(10.0L), 0); break;
    case FN_FRAC:   r = fstore(c, lx - truncl(lx), 0); break;
    case FN_INT:    r = fstore(c, truncl(lx), 0); break;
    case FN_ROUND:
        out->type = RT_INTEGER;
        out->i = fp_to_int64(c, nearbyint(x));
        return;
    case FN_TRUNC:
        out->type = RT_INTEGER;
        out->i = fp_to_int64(c, trunc(x));
        return;
    default:
        break;
    }
    out->type = RT_FLOAT;
    out->f = r;
}

static void eval(Ctx *c, const Node *n, Val *out)
{
    Val rres;

    switch (n->kind) {
    case N_CONST:
        *out = n->cval;
        return;

    case N_VAR:
        out->type = RT_FLOAT;
        out->f = n->id->fval;
        return;

    case N_FUNC:
        eval_builtin(c, n, out);
        return;

    case N_INT2FLOAT:
        eval(c, n->left, out);
        out->f = (double)out->i;
        out->type = RT_FLOAT;
        return;

    case N_IF:
        /* `If Result.ResBoolean` (fpexprpars.pp:3076-3083) is a
           one-byte nonzero test, and root-only Check lets a float-typed
           condition reach it, so this reads byte 0 of the slot. */
        eval(c, n->cond, out);
        eval(c, out->b ? n->left : n->right, out);
        return;

    case N_CASE: {
        /* fpexprpars.pp:3127-3159 */
        Val tag, lab;
        int i = 2;
        int match = 0;

        memset(&tag, 0, sizeof tag);
        memset(&lab, 0, sizeof lab);
        eval(c, n->args[0], &tag);
        while (!match && i < n->nargs) {
            eval(c, n->args[i], &lab);
            switch (tag.type) {
            /* Boolean tags compare the one byte, so a tag and a label
               whose slots differ only above byte 0 still match. */
            case RT_BOOLEAN: match = (tag.b == lab.b); break;
            case RT_INTEGER: match = (tag.i == lab.i); break;
            case RT_STRING:  match = (strcmp(tag.s, lab.s) == 0); break;
            default:         match = (tag.f == lab.f); break;
            }
            if (!match) i += 2;
        }
        /* An odd argument count with no match leaves the Pascal Result
           carrying only a type (fpexprpars.pp:3154-3158); zeroed here so
           the value is at least determinate. Check rejects that shape
           at the root, so it is reachable only under a passing root. */
        memset(out, 0, sizeof *out);
        out->type = node_type(n);
        if (match && i + 1 < n->nargs) eval(c, n->args[i + 1], out);
        else if (!match && (n->nargs % 2) == 0) eval(c, n->args[1], out);
        return;
    }

    case N_NOT:
        eval(c, n->left, out);
        if (out->type == RT_INTEGER) out->i = ~out->i;
        /* `Not` on a Boolean NORMALISES rather than complementing the
           byte: the reference compiles both
           "if(not ((1=0) or (1+84/4503599627370496)),1,2)+0" and the
           0x55 variant to 2, which rules out `xor 1` and `not al`. */
        else if (out->type == RT_BOOLEAN) out->b = (uint8_t)!out->b;
        return;

    case N_NEGATE:
        eval(c, n->left, out);
        if (out->type == RT_INTEGER) out->i = wrap_sub(0, out->i);
        else if (out->type == RT_FLOAT || out->type == RT_CURRENCY)
            out->f = -out->f;
        return;

    case N_AND:
    case N_OR:
    case N_XOR:
        /* No else branch in the Pascal case statement, so a float left
           operand leaves Result holding the LEFT value untouched. */
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        if (out->type == RT_BOOLEAN) {
            /* `and`/`or`/`xor` on Boolean operands are BITWISE on the
               one byte, not logical: the reference compiles
               "if((1=1) and (1+84/4503599627370496),1,2)+0" to 2,
               0x01 AND 0x54 being 0x00. */
            out->b = (n->kind == N_AND) ? (uint8_t)(out->b & rres.b)
                   : (n->kind == N_OR)  ? (uint8_t)(out->b | rres.b)
                                        : (uint8_t)(out->b ^ rres.b);
        } else if (out->type == RT_INTEGER) {
            out->i = (n->kind == N_AND) ? (out->i & rres.i)
                   : (n->kind == N_OR)  ? (out->i | rres.i)
                                        : (out->i ^ rres.i);
        }
        return;

    case N_EQUAL:
    case N_UNEQUAL: {
        int b;

        eval(c, n->left, out);
        eval(c, n->right, &rres);
        switch (out->type) {
        /* One-byte compare, like the boolean operators above:
           "if((1=0)=(1+1/1099511627776),1,2)+0" is 1 in the reference
           (0x00 = 0x00) where a wider read makes it 2. */
        case RT_BOOLEAN: b = (out->b == rres.b); break;
        case RT_INTEGER: b = (out->i == rres.i); break;
        case RT_STRING:  b = (strcmp(out->s, rres.s) == 0); break;
        default:         b = (out->f == rres.f); break;
        }
        if (n->kind == N_UNEQUAL) b = !b;
        val_bool(out, b);
        return;
    }

    case N_LESSTHAN:
    case N_GREATEREQ: {
        int b;

        eval(c, n->left, out);
        eval(c, n->right, &rres);
        switch (out->type) {
        case RT_INTEGER: b = (out->i < rres.i); break;
        case RT_STRING:  b = (strcmp(out->s, rres.s) < 0); break;
        default:         b = (out->f < rres.f); break;
        }
        /* GreaterThanEqual is LessThan negated - fpexprpars.pp:3391 */
        if (n->kind == N_GREATEREQ) b = !b;
        val_bool(out, b);
        return;
    }

    case N_GREATER:
    case N_LESSEQ: {
        int b;

        eval(c, n->left, out);
        eval(c, n->right, &rres);
        switch (out->type) {
        case RT_INTEGER:
            b = (rres.type == RT_INTEGER) ? (out->i > rres.i)
                                          : ((double)out->i > rres.f);
            break;
        case RT_STRING:  b = (strcmp(out->s, rres.s) > 0); break;
        default:
            b = (rres.type == RT_INTEGER) ? (out->f > (double)rres.i)
                                          : (out->f > rres.f);
            break;
        }
        /* LessThanEqual is GreaterThan negated - fpexprpars.pp:3404 */
        if (n->kind == N_LESSEQ) b = !b;
        val_bool(out, b);
        return;
    }

    case N_ADD:
        /* Binary operators are Double arithmetic, not Extended: the
           reference's "sqrt(2)*sqrt(2)-2" is 4.44e-16, the Double
           round-trip answer. Only the math FUNCTIONS carry the x87's
           extra width (see fstore). */
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        if (out->type == RT_INTEGER) {
            out->i = wrap_add(out->i, rres.i);
        } else if (out->type == RT_STRING) {
            size_t la = strlen(out->s);
            size_t lb = strlen(rres.s);
            char *j = (char *)arena_alloc(c->a, la + lb + 1);

            memcpy(j, out->s, la);
            memcpy(j + la, rres.s, lb + 1);
            out->s = j;
        } else if (out->type == RT_FLOAT || out->type == RT_CURRENCY ||
                   out->type == RT_DATETIME) {
            out->f = fchk(c, out->f + rres.f,
                          isfinite(out->f) && isfinite(rres.f));
        }
        out->type = node_type(n);
        return;

    case N_SUB:
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        if (out->type == RT_INTEGER) out->i = wrap_sub(out->i, rres.i);
        else if (out->type == RT_FLOAT || out->type == RT_CURRENCY ||
                 out->type == RT_DATETIME)
            out->f = fchk(c, out->f - rres.f,
                          isfinite(out->f) && isfinite(rres.f));
        return;

    case N_MUL:
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        if (out->type == RT_INTEGER) out->i = wrap_mul(out->i, rres.i);
        else if (out->type == RT_FLOAT || out->type == RT_CURRENCY)
            out->f = fchk(c, out->f * rres.f,
                          isfinite(out->f) && isfinite(rres.f));
        return;

    case N_DIV:
        /* Both arms name rtInteger in the message - fpexprpars.pp:3572
           and 3577, the second one being the 19.59 misnomer. */
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        if (out->type == RT_INTEGER) {
            if (rres.i == 0)
                expr_raise(c, S_DIVISION_BY_ZERO, RT_NAMES[RT_INTEGER]);
            out->f = (double)out->i / (double)rres.i;
        } else {
            if (rres.f == 0.0)
                expr_raise(c, S_DIVISION_BY_ZERO, RT_NAMES[RT_INTEGER]);
            out->f = fchk(c, out->f / rres.f,
                          isfinite(out->f) && isfinite(rres.f));
        }
        out->type = RT_FLOAT;
        return;

    case N_MOD:
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        /* Pascal `mod` truncates toward zero, like C's %, and raises
           the RTL's own division-by-zero (live-probed "7 mod 0"). */
        if (rres.i == 0) expr_raise(c, S_INT_ZERO_DIVIDE);
        if (rres.i == -1) out->i = 0;   /* INT64_MIN % -1 would trap */
        else out->i = out->i % rres.i;
        out->type = RT_INTEGER;
        return;

    case N_POWER:
        eval(c, n->left, out);
        eval(c, n->right, &rres);
        out->f = fp_power(c, arg_to_float(out), arg_to_float(&rres));
        out->type = RT_FLOAT;
        return;
    }
}

/* ------------------------------------------------------------------
   Entry point - Parser.Expression := text; Parser.Evaluate.
   ------------------------------------------------------------------ */
void expr_evaluate(const char *text, const SymbolList *symbols, Arena *a,
                   ExprResult *out)
{
    /* The context lives in the arena, not on the stack: it is written
       right up to the longjmp and read again after it, and only an
       object that is not an automatic local is guaranteed to keep its
       value across setjmp/longjmp. `ctx` itself is set before setjmp
       and never reassigned. */
    Ctx *ctx = (Ctx *)arena_calloc(a, sizeof *ctx);
    Val v;
    Node *root;

    memset(out, 0, sizeof *out);
    memset(&v, 0, sizeof v);
    ctx->src = (text != NULL) ? text : "";
    ctx->len = (long)strlen(ctx->src);
    ctx->pos = (ctx->len == 0) ? 0 : 1;
    ctx->tt = TT_EOF;
    ctx->syms = symbols;
    ctx->a = a;
    ctx->tok = (char *)arena_alloc(a, (size_t)ctx->len + 2);
    ctx->tok[0] = '\0';

    if (setjmp(ctx->jb) != 0) {
        out->kind = EXPR_FAIL;
        memcpy(out->msg, ctx->msg, sizeof out->msg);
        out->msg[sizeof out->msg - 1] = '\0';
        return;
    }

    /* SetExpression (fpexprpars.pp:2062-2079): an empty expression
       builds no node at all and is only reported when Evaluate runs
       (EvaluateExpression, fpexprpars.pp:1595-1600). */
    if (ctx->len == 0) expr_raise(ctx, S_IN_EXPRESSION_EMPTY);

    get_token(ctx);
    root = level1(ctx);
    if (ctx->tt != TT_EOF)
        expr_raise(ctx, S_UNTERM_EXPRESSION, ctx->pos, ctx->tok);
    check_root(ctx, root);

    eval(ctx, root, &v);

    /* Result typing at the call site (USintactic.pas:135-137) treats
       anything but rtInteger/rtFloat as fatal; the engine reports it as
       an outcome and lets the caller compose the text. */
    if (v.type == RT_INTEGER) {
        out->kind = EXPR_INT;
        out->ival = v.i;
    } else if (v.type == RT_FLOAT) {
        out->kind = EXPR_FLOAT;
        out->fval = v.f;
    } else {
        out->kind = EXPR_NONNUM;
    }
}
