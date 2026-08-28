/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/json.h - strict-dialect JSON reader.
   Copyright (C) 2026 Dan Gibson.

   Replaces PHP's json_decode as the back end's input stage. Not a general
   JSON parser: it accepts exactly the dialect DRB's json_encode produces
   and DRF's compiler emits (spec S2), and rejects anything else with a
   positioned error rather than guessing.

   Dialect, in full:
   - Whitespace: space, TAB, CR, LF. CR is data-bearing (CRLF line
     endings appear throughout DRF's JSON output) but does not itself
     advance the line counter - see json_parse below.
   - Strings: full escape set: quote, backslash, forward slash, and the
     letter escapes b f n r t, plus a four-hex-digit "u" escape. A "u"
     escape for a code point below 0x80 decodes to one byte. A code
     point from 0x80 through 0xFF decodes to the same two-byte UTF-8
     pair a raw byte of that value would expand to (see below) - this
     mirrors PHP json_decode reading text that came through utf8_encode
     (drb.php:1732), which is where DRB's JSON actually originates. A
     code point at or above 0x100 is a parse error stating DAAD text is
     single-byte and naming the offending escape.
   - Raw bytes >= 0x80 inside a string are expanded to the two-byte
     UTF-8 pair utf8_encode would have produced: 0x80-0xBF becomes
     C2 xx, 0xC0-0xFF becomes C3 (xx - 0x40). A raw byte >= 0x80 outside
     a string is a parse error.
   - Numbers: optional leading minus, then digits only. A fraction or
     exponent is a parse error naming the strict dialect - DRF never
     emits either.
   - Duplicate object keys: last one wins, matching PHP json_decode.
   - Nesting: guarded at 64 levels (DRF's own output nests 7 deep); the
     65th level is a parse error, not an abort.
   - Errors carry a 1-based line/col and parsing stops at the first one. */
#ifndef NDRC_JSON_H
#define NDRC_JSON_H

#include <stddef.h>
#include "arena.h"
#include "vec.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
VEC_DECLARE(JsonValue, JsonValue *)
struct JsonValue {
    JsonType type;
    long num;                          /* JSON_NUMBER: integers only */
    int boolean;                       /* JSON_BOOL */
    const char *str;                   /* JSON_STRING: binary-safe */
    size_t str_len;                    /* JSON_STRING: may hold any byte < 0x100 */
    Vec_JsonValue *items;               /* JSON_ARRAY */
    Vec_CStr *keys;                     /* JSON_OBJECT: parallel with vals */
    Vec_JsonValue *vals;                /* JSON_OBJECT */
};

typedef struct {
    JsonValue *root;                   /* NULL on failure */
    int ok;
    int line, col;                     /* 1-based error position; 0 on success */
    char err[128];                     /* empty on success */
} JsonResult;

/* Parses data[0..n). All output is allocated from a and lives as long as
   a does. data need not be NUL-terminated - n is authoritative. */
JsonResult json_parse(Arena *a, const unsigned char *data, size_t n);

/* Looks up key in a JSON_OBJECT. NULL if obj is not an object or the key
   is absent. Scans back to front, so a duplicate key's last occurrence
   is what a caller sees, matching PHP json_decode. */
JsonValue *json_get(const JsonValue *obj, const char *key);

#endif /* NDRC_JSON_H */
