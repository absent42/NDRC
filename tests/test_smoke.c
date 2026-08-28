/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_smoke.c - proves the harness reports failures.
   Copyright (C) 2026 Dan Gibson. */
#include "test.h"

TEST(smoke_arithmetic)
{
    CHECK_INT(2 + 2, 4);
}

int main(void)
{
    RUN(smoke_arithmetic);
    return test_summary("smoke");
}
