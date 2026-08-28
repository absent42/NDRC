/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_emit_hdr.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "arena.h"
#include "back/emit_hdr.h"
#include "model.h"
#include "str.h"
#include "targets.h"

#include <string.h>

/* PORT: drb.php:1837-1840 -
   $b = 2;
   if ($v3code) $b = 3;
   writeByte($outputFileHandler, $b);
   emit_hdr.c's version local already reads `adv->v3code ? 3u : 2u`
   (emit_hdr.c:11) - this test pins that byte rather than changing it.

   Golden cross-check, measured directly from the committed fixtures:
   tests/goldens/BLANK_EN/ST_EN_v2_opt.ddb and .../ST_EN_v3_opt.ddb are
   both 2112 bytes and differ in exactly one byte - offset 0, 0x02 vs
   0x03 - confirming the version byte is DDB byte 0 and the only
   difference a v2/v3 compile of the same source makes to this
   fixture. */
static Adventure make_adventure(int v3code, Arena *a)
{
    Adventure adv;
    memset(&adv, 0, sizeof(adv));
    adv.v3code = v3code;
    adv.object_data = vec_new_ObjectData(a);
    adv.locations = vec_new_Message(a);
    adv.messages = vec_new_Message(a);
    adv.sysmess = vec_new_Message(a);
    adv.processes = vec_new_Process(a);
    return adv;
}

TEST(version_byte_v2_is_2)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target *t = target_lookup("ST", NULL);
    Adventure adv = make_adventure(0, a);
    long addr = 0;

    CHECK(t != NULL);
    emit_header(out, &addr, t, &adv, 0);

    CHECK_INT(str_bytes(out)[0], 2);
    arena_free(a);
}

TEST(version_byte_v3_is_3)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    const Target *t = target_lookup("ST", NULL);
    Adventure adv = make_adventure(1, a);
    long addr = 0;

    CHECK(t != NULL);
    emit_header(out, &addr, t, &adv, 0);

    CHECK_INT(str_bytes(out)[0], 3);
    arena_free(a);
}

int main(void)
{
    RUN(version_byte_v2_is_2);
    RUN(version_byte_v3_is_3);
    return test_summary("emit_hdr");
}
