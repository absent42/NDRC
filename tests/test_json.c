/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_json.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "json.h"
#include "vec.h"

#include <string.h>

static JsonResult parse_str(Arena *a, const char *s)
{
    return json_parse(a, (const unsigned char *)s, strlen(s));
}

TEST(json_parses_object_array_number_string_bool_null)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a,
        "{\"a\":[1,-2],\"b\":\"x\",\"c\":true,\"d\":null}");
    JsonValue *arr, *b, *c, *d;

    CHECK_INT(r.ok, 1);
    CHECK_INT(r.root->type, JSON_OBJECT);

    arr = json_get(r.root, "a");
    CHECK(arr != NULL);
    CHECK_INT(arr->type, JSON_ARRAY);
    CHECK_INT(vec_len_JsonValue(arr->items), 2);
    CHECK_INT(vec_at_JsonValue(arr->items, 0)->num, 1);
    CHECK_INT(vec_at_JsonValue(arr->items, 1)->num, -2);

    b = json_get(r.root, "b");
    CHECK(b != NULL);
    CHECK_INT(b->type, JSON_STRING);
    CHECK_INT(b->str_len, 1);
    CHECK_INT(b->str[0], 'x');

    c = json_get(r.root, "c");
    CHECK(c != NULL);
    CHECK_INT(c->type, JSON_BOOL);
    CHECK_INT(c->boolean, 1);

    d = json_get(r.root, "d");
    CHECK(d != NULL);
    CHECK_INT(d->type, JSON_NULL);

    arena_free(a);
}

TEST(json_object_lookup_returns_value_or_null)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a, "{\"x\":1}");
    JsonValue *x;

    CHECK_INT(r.ok, 1);
    x = json_get(r.root, "x");
    CHECK(x != NULL);
    CHECK_INT(x->num, 1);
    CHECK(json_get(r.root, "y") == NULL);
    /* A non-object value has no keys to look up. */
    CHECK(json_get(x, "x") == NULL);
    CHECK(json_get(NULL, "x") == NULL);

    arena_free(a);
}

TEST(json_duplicate_keys_last_wins)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a, "{\"k\":1,\"k\":2}");
    JsonValue *k;

    CHECK_INT(r.ok, 1);
    k = json_get(r.root, "k");
    CHECK(k != NULL);
    CHECK_INT(k->num, 2);
    /* Both entries are kept - json_get resolves the winner by scanning
       backwards, with nothing deleted at parse time. */
    CHECK_INT(vec_len_CStr(r.root->keys), 2);

    arena_free(a);
}

TEST(json_crlf_and_tabs_are_whitespace)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a, "{\r\n\t\"a\" : 1\r\n}");

    CHECK_INT(r.ok, 1);
    CHECK_INT(json_get(r.root, "a")->num, 1);

    arena_free(a);
}

TEST(json_standard_escapes_decode)
{
    Arena *a = arena_new(0);
    /* JSON source bytes for the string "\" \\ \/ \b \f \n \r \t"
       (concatenated with no separators), built as a byte array so no
       C escape sequence has to encode a JSON escape sequence. */
    static const unsigned char raw[] = {
        '"',
        '\\', '"',
        '\\', '\\',
        '\\', '/',
        '\\', 'b',
        '\\', 'f',
        '\\', 'n',
        '\\', 'r',
        '\\', 't',
        '"'
    };
    static const unsigned char want[] = {
        '"', '\\', '/', 0x08, 0x0C, 0x0A, 0x0D, 0x09
    };
    JsonResult r = json_parse(a, raw, sizeof(raw));

    CHECK_INT(r.ok, 1);
    CHECK_INT(r.root->type, JSON_STRING);
    CHECK_INT(r.root->str_len, sizeof(want));
    CHECK_MEM(r.root->str, want, sizeof(want));

    arena_free(a);
}

TEST(json_u_escape_below_0x80_decodes_to_one_byte)
{
    Arena *a = arena_new(0);
    /* "" as raw bytes, avoiding C's own \u universal-character-name
       escape entirely. */
    static const unsigned char raw[] = {
        '"', '\\', 'u', '0', '0', '1', '5', '"'
    };
    JsonResult r = json_parse(a, raw, sizeof(raw));

    CHECK_INT(r.ok, 1);
    CHECK_INT(r.root->type, JSON_STRING);
    CHECK_INT(r.root->str_len, 1);
    CHECK_INT((unsigned char)r.root->str[0], 0x15);

    arena_free(a);
}

TEST(json_u_escape_0x80_to_0xff_expands_to_utf8_pair)
{
    Arena *a = arena_new(0);
    static const unsigned char raw_e9[] = {
        '"', '\\', 'u', '0', '0', 'E', '9', '"'
    };
    static const unsigned char raw_a9[] = {
        '"', '\\', 'u', '0', '0', 'A', '9', '"'
    };
    JsonResult r1 = json_parse(a, raw_e9, sizeof(raw_e9));
    JsonResult r2 = json_parse(a, raw_a9, sizeof(raw_a9));

    CHECK_INT(r1.ok, 1);
    CHECK_INT(r1.root->str_len, 2);
    CHECK_INT((unsigned char)r1.root->str[0], 0xC3);
    CHECK_INT((unsigned char)r1.root->str[1], 0xA9);

    CHECK_INT(r2.ok, 1);
    CHECK_INT(r2.root->str_len, 2);
    CHECK_INT((unsigned char)r2.root->str[0], 0xC2);
    CHECK_INT((unsigned char)r2.root->str[1], 0xA9);

    arena_free(a);
}

TEST(json_u_escape_at_or_above_0x100_is_error)
{
    Arena *a = arena_new(0);
    static const unsigned char raw[] = {
        '"', '\\', 'u', '0', '1', '0', '0', '"'
    };
    JsonResult r = json_parse(a, raw, sizeof(raw));

    CHECK_INT(r.ok, 0);
    CHECK(strstr(r.err, "single-byte") != NULL);

    arena_free(a);
}

TEST(json_high_raw_bytes_expand_like_utf8_encode)
{
    Arena *a = arena_new(0);
    static const unsigned char raw_e9[] = { '"', 0xE9, '"' };
    static const unsigned char raw_a9[] = { '"', 0xA9, '"' };
    JsonResult r1 = json_parse(a, raw_e9, sizeof(raw_e9));
    JsonResult r2 = json_parse(a, raw_a9, sizeof(raw_a9));

    CHECK_INT(r1.ok, 1);
    CHECK_INT(r1.root->str_len, 2);
    CHECK_INT((unsigned char)r1.root->str[0], 0xC3);
    CHECK_INT((unsigned char)r1.root->str[1], 0xA9);

    CHECK_INT(r2.ok, 1);
    CHECK_INT(r2.root->str_len, 2);
    CHECK_INT((unsigned char)r2.root->str[0], 0xC2);
    CHECK_INT((unsigned char)r2.root->str[1], 0xA9);

    arena_free(a);
}

TEST(json_fraction_is_error)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a, "1.5");

    CHECK_INT(r.ok, 0);
    CHECK(strstr(r.err, "fraction") != NULL);

    arena_free(a);
}

/* Exercises the documented line/col semantics: the embedded LF after
   '{' increments the line and resets the column, and every ordinary
   byte after that advances the column by one, so the reported EOF
   position is hand-checkable. */
TEST(json_unterminated_string_reports_line_col)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a, "{\n \"key\": \"broken");

    CHECK_INT(r.ok, 0);
    CHECK(strstr(r.err, "unterminated") != NULL);
    CHECK_INT(r.line, 2);
    CHECK_INT(r.col, 16);

    arena_free(a);
}

TEST(json_trailing_garbage_is_error)
{
    Arena *a = arena_new(0);
    JsonResult r = parse_str(a, "{} x");

    CHECK_INT(r.ok, 0);
    CHECK(strstr(r.err, "trailing") != NULL);

    arena_free(a);
}

TEST(json_binary_safe_embedded_nul)
{
    Arena *a = arena_new(0);
    static const unsigned char raw[] = {
        '"', '\\', 'u', '0', '0', '0', '0', '"'
    };
    JsonResult r = json_parse(a, raw, sizeof(raw));

    CHECK_INT(r.ok, 1);
    CHECK_INT(r.root->str_len, 1);
    CHECK_INT((unsigned char)r.root->str[0], 0);

    arena_free(a);
}

TEST(json_nesting_guard_trips_at_65_not_64)
{
    Arena *a = arena_new(0);
    char buf[256];
    size_t len;
    int i;
    JsonResult r;

    len = 0;
    for (i = 0; i < 64; i++) buf[len++] = '[';
    for (i = 0; i < 64; i++) buf[len++] = ']';
    r = json_parse(a, (const unsigned char *)buf, len);
    CHECK_INT(r.ok, 1);

    len = 0;
    for (i = 0; i < 65; i++) buf[len++] = '[';
    for (i = 0; i < 65; i++) buf[len++] = ']';
    r = json_parse(a, (const unsigned char *)buf, len);
    CHECK_INT(r.ok, 0);
    CHECK(strstr(r.err, "nesting") != NULL);

    arena_free(a);
}

int main(void)
{
    RUN(json_parses_object_array_number_string_bool_null);
    RUN(json_object_lookup_returns_value_or_null);
    RUN(json_duplicate_keys_last_wins);
    RUN(json_crlf_and_tabs_are_whitespace);
    RUN(json_standard_escapes_decode);
    RUN(json_u_escape_below_0x80_decodes_to_one_byte);
    RUN(json_u_escape_0x80_to_0xff_expands_to_utf8_pair);
    RUN(json_u_escape_at_or_above_0x100_is_error);
    RUN(json_high_raw_bytes_expand_like_utf8_encode);
    RUN(json_fraction_is_error);
    RUN(json_unterminated_string_reports_line_col);
    RUN(json_trailing_garbage_is_error);
    RUN(json_binary_safe_embedded_nul);
    RUN(json_nesting_guard_trips_at_65_not_64);
    return test_summary("json");
}
