/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_emit_msg.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "back/emit.h"
#include "model.h"
#include "str.h"
#include "targets.h"

#include <stdio.h>
#include <string.h>

/* NEXTDAAD: little-endian, not a padding platform, so layout_pad is a no-op. */
static const Target *nextdaad(void)
{
    return target_lookup("NEXTDAAD", NULL);
}

static Vec_Message *make_table(Arena *a, const char **texts, size_t n)
{
    Vec_Message *v = vec_new_Message(a);
    size_t i;
    for (i = 0; i < n; i++) {
        Message *m = arena_calloc(a, sizeof(*m));
        m->Value = (long)i;
        m->Text = str_from(a, texts[i]);
        vec_push_Message(v, m);
    }
    return v;
}

TEST(share_off_is_byte_identical)
{
    static const unsigned char want[] = {
        0xBE, 0xF5, 0xBD, 0xF5, 0xBE, 0xF5,
        0x00, 0x00, 0x02, 0x00, 0x04, 0x00
    };
    const char *texts[] = { "A", "B", "A" };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target *t = nextdaad();
    long addr = 0;

    CHECK(t != NULL);
    emit_messages(out, &addr, t, make_table(a, texts, 3), 0, 0, NULL, NULL, 0, NULL);

    CHECK_INT(str_len(out), sizeof(want));
    CHECK_MEM(str_bytes(out), want, sizeof(want));
    CHECK_INT(addr, sizeof(want));
    arena_free(a);
}

TEST(share_on_repeats_lookup_word)
{
    static const unsigned char want[] = {
        0xBE, 0xF5, 0xBD, 0xF5,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00
    };
    const char *texts[] = { "A", "B", "A" };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Str *out2 = str_new(a);
    const Target *t = nextdaad();
    ShareStats stats = { 0, 0 };
    long addr = 0, addr2 = 0;

    emit_messages(out, &addr, t, make_table(a, texts, 3), 0, 0, NULL, NULL, 1, &stats);

    CHECK_INT(str_len(out), sizeof(want));
    CHECK_MEM(str_bytes(out), want, sizeof(want));
    CHECK_INT(addr, sizeof(want));
    CHECK_INT(stats.texts, 1);
    CHECK_INT(stats.bytes, 2);

    /* NULL stats: same bytes. */
    emit_messages(out2, &addr2, t, make_table(a, texts, 3), 0, 0, NULL, NULL, 1, NULL);
    CHECK_INT(str_len(out2), sizeof(want));
    CHECK_MEM(str_bytes(out2), want, sizeof(want));
    arena_free(a);
}

TEST(share_first_occurrence_keeps_copy)
{
    static const unsigned char want[] = {
        0xBE, 0xF5,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    const char *texts[] = { "A", "A", "A" };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    ShareStats stats = { 0, 0 };
    long addr = 0;

    emit_messages(out, &addr, nextdaad(), make_table(a, texts, 3), 0, 0, NULL, NULL, 1, &stats);

    CHECK_INT(str_len(out), sizeof(want));
    CHECK_MEM(str_bytes(out), want, sizeof(want));
    CHECK_INT(stats.texts, 2);
    CHECK_INT(stats.bytes, 4);
    arena_free(a);
}

TEST(share_empty_texts)
{
    static const unsigned char want[] = {
        0xF5,
        0x00, 0x00, 0x00, 0x00
    };
    const char *texts[] = { "", "" };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    ShareStats stats = { 0, 0 };
    long addr = 0;

    emit_messages(out, &addr, nextdaad(), make_table(a, texts, 2), 0, 0, NULL, NULL, 1, &stats);

    CHECK_INT(str_len(out), sizeof(want));
    CHECK_MEM(str_bytes(out), want, sizeof(want));
    CHECK_INT(stats.texts, 1);
    CHECK_INT(stats.bytes, 1);
    arena_free(a);
}

/* STX entries 0-62 stay in RAM under -X; 63 and 64 go to the XMB.
   Entry 64 repeats entry 10's text across that boundary. */
TEST(share_never_crosses_destinations)
{
    static const unsigned char want_xmb[] = {
        'M' ^ 0xFF, '6' ^ 0xFF, '3' ^ 0xFF, 0xF5,
        'M' ^ 0xFF, '1' ^ 0xFF, '0' ^ 0xFF, 0xF5
    };
    const char *texts[65];
    char buf[65][4];
    unsigned char got_xmb[sizeof(want_xmb) + 1];
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    ShareStats stats = { 0, 0 };
    FILE *xmb = tmpfile();
    long addr = 0, xmb_addr = 0;
    const unsigned char *b;
    size_t i, got;

    for (i = 0; i < 65; i++) {
        snprintf(buf[i], sizeof(buf[i]), "M%02u", (unsigned)i);
        texts[i] = buf[i];
    }
    texts[64] = texts[10];

    CHECK(xmb != NULL);
    if (xmb == NULL) {
        arena_free(a);
        return;
    }
    emit_messages(out, &addr, nextdaad(), make_table(a, texts, 65), 1, 1, xmb, &xmb_addr, 1, &stats);

    /* 63 RAM texts of 4 bytes, then 65 words. */
    CHECK_INT(str_len(out), 63 * 4 + 65 * 2);
    b = str_bytes(out) + 63 * 4;
    CHECK_INT(b[10 * 2] | (b[10 * 2 + 1] << 8), 40);
    CHECK_INT(b[63 * 2] | (b[63 * 2 + 1] << 8), 0);
    CHECK_INT(b[64 * 2] | (b[64 * 2 + 1] << 8), 4);
    CHECK_INT(xmb_addr, sizeof(want_xmb));
    CHECK_INT(stats.texts, 0);
    CHECK_INT(stats.bytes, 0);

    rewind(xmb);
    got = fread(got_xmb, 1, sizeof(got_xmb), xmb);
    CHECK_INT(got, sizeof(want_xmb));
    CHECK_MEM(got_xmb, want_xmb, sizeof(want_xmb));
    fclose(xmb);
    arena_free(a);
}

int main(void)
{
    RUN(share_off_is_byte_identical);
    RUN(share_on_repeats_lookup_word);
    RUN(share_first_occurrence_keeps_copy);
    RUN(share_empty_texts);
    RUN(share_never_crosses_destinations);
    return test_summary("emit_msg");
}
