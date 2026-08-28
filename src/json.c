/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/json.c - strict-dialect JSON reader.
   Copyright (C) 2026 Dan Gibson.

   Recursive-descent parser over data[0..n). Every recursive step checks
   p->failed before doing further work, so the first error reported wins
   and no code path can overwrite it - that is what makes "parsing stops
   at the first error" true without every call site checking a return
   value it already has (NULL) to check.

   Line/col tracking (json_parse's contract): line starts at 1, col
   starts at 1. Consuming a line-feed byte increments line and resets
   col to 1. Consuming a carriage-return byte resets col to 1 but does
   NOT increment line. The two rules together give CRLF the right
   result: the CR resets col (a no-op ahead of the LF that follows), and
   the LF does the actual line increment - so a CRLF pair counts once,
   exactly as a bare LF would, while a lone CR never advances the line
   counter on its own. */
#include "json.h"
#include "str.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    Arena *a;
    const unsigned char *data;
    size_t n;
    size_t pos;
    int line, col;
    int depth;
    int failed;
    int err_line, err_col;
    char err[128];
} P;

static JsonValue *parse_value(P *p);
static JsonValue *parse_object(P *p);
static JsonValue *parse_array(P *p);
static JsonValue *parse_string(P *p);
static JsonValue *parse_number(P *p);
static JsonValue *parse_literal(P *p, const char *lit, JsonType t, int boolval);
static int parse_raw_string(P *p, Str **out);

static int peek(const P *p)
{
    if (p->pos >= p->n) return -1;
    return p->data[p->pos];
}

/* Advances over the byte peek() just returned, updating line/col per the
   file header comment. Never call this at end of input. */
static void adv(P *p)
{
    unsigned char c = p->data[p->pos];
    p->pos++;
    if (c == '\n') {
        p->line++;
        p->col = 1;
    } else if (c == '\r') {
        p->col = 1;
    } else {
        p->col++;
    }
}

/* Records the first failure only: later fail() calls in the unwind back
   up the call stack are no-ops, which is what keeps the reported error
   the FIRST one encountered rather than the last. */
static void fail(P *p, const char *fmt, ...)
{
    va_list ap;

    if (p->failed) return;
    p->failed = 1;
    p->err_line = p->line;
    p->err_col = p->col;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
}

static void skip_ws(P *p)
{
    for (;;) {
        int c = peek(p);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        adv(p);
    }
}

static JsonValue *new_value(P *p, JsonType t)
{
    JsonValue *v = arena_calloc(p->a, sizeof(*v));
    v->type = t;
    return v;
}

/* Pushes the two-byte UTF-8 pair for a byte value b in [0x80, 0xFF],
   matching drb.php:1732's utf8_encode: 0x80-0xBF becomes C2 xx, and
   0xC0-0xFF becomes C3 (xx - 0x40). Shared by raw high bytes in a
   string and by the u-escape range 0x80-0xFF (ruling P1), since both
   must land on exactly the same pair. */
static void push_high_byte(Str *s, unsigned b)
{
    if (b <= 0xBF) {
        str_push(s, (char)0xC2);
        str_push(s, (char)b);
    } else {
        str_push(s, (char)0xC3);
        str_push(s, (char)(b - 0x40u));
    }
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses a JSON string body (opening quote through closing quote) into
   a fresh Str. Shared by string values and object keys, since both obey
   the same escape and high-byte rules. */
static int parse_raw_string(P *p, Str **out)
{
    Str *s;

    if (peek(p) != '"') {
        fail(p, "expected string");
        return 0;
    }
    adv(p);

    s = str_new(p->a);
    for (;;) {
        int c = peek(p);

        if (c < 0) {
            fail(p, "unterminated string");
            return 0;
        }
        if (c == '"') {
            adv(p);
            break;
        }
        if (c == '\\') {
            int e;
            adv(p);
            e = peek(p);
            if (e < 0) {
                fail(p, "unterminated string");
                return 0;
            }
            switch (e) {
            case '"':  str_push(s, '"');  adv(p); break;
            case '\\': str_push(s, '\\'); adv(p); break;
            case '/':  str_push(s, '/');  adv(p); break;
            case 'b':  str_push(s, (char)0x08); adv(p); break;
            case 'f':  str_push(s, (char)0x0C); adv(p); break;
            case 'n':  str_push(s, (char)0x0A); adv(p); break;
            case 'r':  str_push(s, (char)0x0D); adv(p); break;
            case 't':  str_push(s, (char)0x09); adv(p); break;
            case 'u': {
                unsigned val = 0;
                int i;
                adv(p); /* consume 'u' */
                for (i = 0; i < 4; i++) {
                    int d = hex_digit(peek(p));
                    if (d < 0) {
                        fail(p, "invalid u-escape, expected four hex digits");
                        return 0;
                    }
                    val = (val << 4) | (unsigned)d;
                    adv(p);
                }
                if (val >= 0x100u) {
                    fail(p,
                         "DAAD text is single-byte: u-escape for code point "
                         "0x%04X exceeds one byte", val);
                    return 0;
                }
                if (val < 0x80u) {
                    str_push(s, (char)val);
                } else {
                    push_high_byte(s, val);
                }
                break;
            }
            default:
                fail(p, "unknown string escape '\\%c'", (char)e);
                return 0;
            }
            continue;
        }
        if ((unsigned char)c >= 0x80u) {
            unsigned b = (unsigned char)c;
            adv(p);
            push_high_byte(s, b);
            continue;
        }
        str_push(s, (char)c);
        adv(p);
    }

    *out = s;
    return 1;
}

static JsonValue *parse_string(P *p)
{
    Str *s;
    JsonValue *v;

    if (!parse_raw_string(p, &s)) return NULL;
    v = new_value(p, JSON_STRING);
    v->str = (const char *)str_bytes(s);
    v->str_len = str_len(s);
    return v;
}

static JsonValue *parse_number(P *p)
{
    int neg = 0;
    long long val = 0;
    int any = 0;
    JsonValue *v;

    if (peek(p) == '-') {
        neg = 1;
        adv(p);
    }
    while (peek(p) >= '0' && peek(p) <= '9') {
        int d = peek(p) - '0';
        val = val * 10 + d;
        any = 1;
        adv(p);
        /* Guard at 2^31-1: every value this dialect actually carries is
           under 65536, so this only ever fires on malformed input. */
        if (val > 2147483647LL) {
            fail(p, "number too large for the strict dialect");
            return NULL;
        }
    }
    if (!any) {
        fail(p, "invalid number");
        return NULL;
    }
    if (peek(p) == '.' || peek(p) == 'e' || peek(p) == 'E') {
        fail(p,
             "fraction/exponent not allowed: strict dialect is integers "
             "only");
        return NULL;
    }

    v = new_value(p, JSON_NUMBER);
    v->num = (long)(neg ? -val : val);
    return v;
}

static JsonValue *parse_literal(P *p, const char *lit, JsonType t, int boolval)
{
    size_t len = strlen(lit);
    size_t i;
    JsonValue *v;

    for (i = 0; i < len; i++) {
        if (peek(p) != (unsigned char)lit[i]) {
            fail(p, "invalid literal, expected '%s'", lit);
            return NULL;
        }
        adv(p);
    }
    v = new_value(p, t);
    if (t == JSON_BOOL) v->boolean = boolval;
    return v;
}

static JsonValue *parse_array(P *p)
{
    JsonValue *v;

    adv(p); /* consume '[' */
    p->depth++;
    if (p->depth > 64) {
        fail(p, "nesting too deep");
        p->depth--;
        return NULL;
    }

    v = new_value(p, JSON_ARRAY);
    v->items = vec_new_JsonValue(p->a);

    skip_ws(p);
    if (peek(p) == ']') {
        adv(p);
        p->depth--;
        return v;
    }

    for (;;) {
        JsonValue *item;

        skip_ws(p);
        item = parse_value(p);
        if (p->failed) {
            p->depth--;
            return NULL;
        }
        vec_push_JsonValue(v->items, item);

        skip_ws(p);
        if (peek(p) == ',') {
            adv(p);
            continue;
        }
        if (peek(p) == ']') {
            adv(p);
            break;
        }
        fail(p, "expected ',' or ']' in array");
        p->depth--;
        return NULL;
    }

    p->depth--;
    return v;
}

static JsonValue *parse_object(P *p)
{
    JsonValue *v;

    adv(p); /* consume '{' */
    p->depth++;
    if (p->depth > 64) {
        fail(p, "nesting too deep");
        p->depth--;
        return NULL;
    }

    v = new_value(p, JSON_OBJECT);
    v->keys = vec_new_CStr(p->a);
    v->vals = vec_new_JsonValue(p->a);

    skip_ws(p);
    if (peek(p) == '}') {
        adv(p);
        p->depth--;
        return v;
    }

    for (;;) {
        Str *key;
        JsonValue *val;

        skip_ws(p);
        if (!parse_raw_string(p, &key)) {
            p->depth--;
            return NULL;
        }
        skip_ws(p);
        if (peek(p) != ':') {
            fail(p, "expected ':' in object");
            p->depth--;
            return NULL;
        }
        adv(p);
        skip_ws(p);
        val = parse_value(p);
        if (p->failed) {
            p->depth--;
            return NULL;
        }
        vec_push_CStr(v->keys, str_cstr(key));
        vec_push_JsonValue(v->vals, val);

        skip_ws(p);
        if (peek(p) == ',') {
            adv(p);
            continue;
        }
        if (peek(p) == '}') {
            adv(p);
            break;
        }
        fail(p, "expected ',' or '}' in object");
        p->depth--;
        return NULL;
    }

    p->depth--;
    return v;
}

static JsonValue *parse_value(P *p)
{
    int c;

    if (p->failed) return NULL;
    c = peek(p);
    if (c < 0) {
        fail(p, "unexpected end of input");
        return NULL;
    }
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '"') return parse_string(p);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (c == 't') return parse_literal(p, "true", JSON_BOOL, 1);
    if (c == 'f') return parse_literal(p, "false", JSON_BOOL, 0);
    if (c == 'n') return parse_literal(p, "null", JSON_NULL, 0);
    if ((unsigned char)c >= 0x80u) {
        fail(p, "raw byte 0x%02X not allowed outside a string",
             (unsigned char)c);
        return NULL;
    }
    fail(p, "unexpected character '%c'", (char)c);
    return NULL;
}

JsonResult json_parse(Arena *a, const unsigned char *data, size_t n)
{
    P p;
    JsonResult r;
    JsonValue *root;

    p.a = a;
    p.data = data;
    p.n = n;
    p.pos = 0;
    p.line = 1;
    p.col = 1;
    p.depth = 0;
    p.failed = 0;
    p.err_line = 1;
    p.err_col = 1;
    p.err[0] = '\0';

    skip_ws(&p);
    root = parse_value(&p);
    if (!p.failed) {
        skip_ws(&p);
        if (p.pos != p.n) fail(&p, "trailing garbage after JSON value");
    }

    if (p.failed) {
        r.root = NULL;
        r.ok = 0;
        r.line = p.err_line;
        r.col = p.err_col;
        memcpy(r.err, p.err, sizeof(r.err));
    } else {
        r.root = root;
        r.ok = 1;
        r.line = 0;
        r.col = 0;
        r.err[0] = '\0';
    }
    return r;
}

JsonValue *json_get(const JsonValue *obj, const char *key)
{
    size_t i;

    if (obj == NULL || obj->type != JSON_OBJECT) return NULL;

    /* Scans back to front so the LAST duplicate wins without deleting
       earlier entries - matches PHP json_decode, and needs no special
       case at insertion time since parse_object just appends. */
    for (i = vec_len_CStr(obj->keys); i > 0; i--) {
        const char *k = vec_at_CStr(obj->keys, i - 1);
        if (k != NULL && strcmp(k, key) == 0) {
            return vec_at_JsonValue(obj->vals, i - 1);
        }
    }
    return NULL;
}
