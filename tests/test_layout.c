/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_layout.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "layout.h"
#include "str.h"
#include "targets.h"

TEST(pads_padding_platform_at_odd_address)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target t = { .padding_platform = 1 };
    long addr = 5;

    layout_pad(out, &addr, &t);

    CHECK_INT(addr, 6);
    CHECK_INT(str_len(out), 1);
    CHECK_INT(str_bytes(out)[0], 0);
    arena_free(a);
}

TEST(no_pad_padding_platform_at_even_address)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target t = { .padding_platform = 1 };
    long addr = 4;

    layout_pad(out, &addr, &t);

    CHECK_INT(addr, 4);
    CHECK_INT(str_len(out), 0);
    arena_free(a);
}

TEST(nextdaad_never_pads_even_at_odd_address)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target *t = target_lookup("NEXTDAAD", NULL);
    long addr = 5;

    CHECK(t != NULL);
    layout_pad(out, &addr, t);

    CHECK_INT(addr, 5);
    CHECK_INT(str_len(out), 0);
    arena_free(a);
}

TEST(forced_padding_pads_non_padding_platform_at_odd_address)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target t = { .padding_platform = 0 };
    long addr = 5;

    /* PORT: drb.php:291's `$adventure->forcedPadding` OR-term - -P
       forces padding on a target that would not otherwise pad (the
       `(t->padding_platform || forced_padding)` condition). Reset to
       (0,0,...) afterward so no later test in this binary (or a
       future one appended after it) inherits this test's forced
       state - layout_set_forced's write context is a module static. */
    layout_set_forced(1, 0, out, NULL);
    layout_pad(out, &addr, &t);
    layout_set_forced(0, 0, NULL, NULL);

    CHECK_INT(addr, 6);
    CHECK_INT(str_len(out), 1);
    CHECK_INT(str_bytes(out)[0], 0);
    arena_free(a);
}

TEST(forced_padding_off_leaves_non_padding_platform_alone)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target t = { .padding_platform = 0 };
    long addr = 5;

    layout_pad(out, &addr, &t);

    CHECK_INT(addr, 5);
    CHECK_INT(str_len(out), 0);
    arena_free(a);
}

int main(void)
{
    RUN(pads_padding_platform_at_odd_address);
    RUN(no_pad_padding_platform_at_even_address);
    RUN(nextdaad_never_pads_even_at_odd_address);
    RUN(forced_padding_pads_non_padding_platform_at_odd_address);
    RUN(forced_padding_off_leaves_non_padding_platform_alone);
    return test_summary("layout");
}
