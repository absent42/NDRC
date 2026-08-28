/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_targets.c - Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include "targets.h"

/* Counts: 14 bare targets (drb.php:1233-1236 isValidTarget) minus 4 that
   split into subtargets (drb.php:1238-1247 isValidSubtarget: MSX2, PC,
   ZX, ZX81), plus 6 (ZX) + 2 (ZX81) + 5 (PC) + 12 (MSX2) subtarget rows
   = 10 bare + 25 subtargeted = 35 rows total. */
TEST(row_count_is_35)
{
    static const char *bare_names[] = {
        "NEXTDAAD", "C64", "CPC", "CP4", "CPM", "MSX", "PCW", "AMIGA",
        "ST", "HTML"
    };
    static const char *zx_subs[] = {
        "48K", "128K", "PLUS3", "ESXDOS", "NEXT", "UNO"
    };
    static const char *zx81_subs[] = { "16K", "SD81B" };
    static const char *pc_subs[] = { "VGA256", "VGA", "CGA", "EGA", "TEXT" };
    static const char *msx2_subs[] = {
        "5_6", "5_8", "6_6", "6_8", "7_6", "7_8",
        "8_6", "8_8", "10_6", "10_8", "12_6", "12_8"
    };
    int count = 0;
    size_t i;

    for (i = 0; i < sizeof(bare_names) / sizeof(bare_names[0]); i++)
        if (target_lookup(bare_names[i], NULL) != NULL) count++;
    for (i = 0; i < sizeof(zx_subs) / sizeof(zx_subs[0]); i++)
        if (target_lookup("ZX", zx_subs[i]) != NULL) count++;
    for (i = 0; i < sizeof(zx81_subs) / sizeof(zx81_subs[0]); i++)
        if (target_lookup("ZX81", zx81_subs[i]) != NULL) count++;
    for (i = 0; i < sizeof(pc_subs) / sizeof(pc_subs[0]); i++)
        if (target_lookup("PC", pc_subs[i]) != NULL) count++;
    for (i = 0; i < sizeof(msx2_subs) / sizeof(msx2_subs[0]); i++)
        if (target_lookup("MSX2", msx2_subs[i]) != NULL) count++;

    /* Every one of the 35 rows (10 bare + 6 ZX + 2 ZX81 + 5 PC + 12
       MSX2 subtargets) must resolve, and nothing beyond them should be
       counted - this asserts the row count directly rather than
       trusting the bare-target loop alone to stand in for it. */
    CHECK_INT(count, 35);
}

TEST(nextdaad_row_matches_reference)
{
    const Target *t = target_lookup("NEXTDAAD", NULL);
    CHECK(t != NULL);
    CHECK_INT(t->machine_id, 0x0C);          /* drb.php:1280 */
    CHECK_INT(t->submachine_id, 95);         /* drb.php:1261 default arm */
    CHECK_INT(t->base_address, 0x0000);      /* drb.php:1297 */
    CHECK_INT(t->big_endian, 0);             /* drb.php:1310-1313 */
    CHECK_INT(t->padding_platform, 0);       /* drb.php:1305-1308 */
    CHECK_INT(t->duration_base_length, 100); /* drb.php:1537 */
    CHECK_INT(t->beep_swap, 1);              /* drb.php:906-911 */
    CHECK_INT(t->debug_allowed, 1);          /* drb.php:1802 */
    CHECK_INT(t->xmessage_size_k, 64);       /* drb.php:426 */
}

/* Machine-ID traps (drb.php:1265-1281, verified against goldens:
   ST_EN_v3_opt byte1=0x50, ZX_48K=0x10, MSX2_12_8=0xF0, HTML=0xD0 -
   these are (machine_id<<4)|submachine-nibble style header bytes, the
   high nibble is machine_id). */
TEST(machine_id_traps)
{
    /* drb.php:1267 - PC returns 0x00 unconditionally on the FIRST line
       of the function, before the VGA256/HTML 0x0D check is ever
       reached, so PC+VGA256 is 0x00 not 0x0D. */
    CHECK_INT(target_lookup("PC", "VGA256")->machine_id, 0x00);
    CHECK_INT(target_lookup("HTML", NULL)->machine_id, 0x0D);   /* drb.php:1277 */
    CHECK_INT(target_lookup("ZX", "48K")->machine_id, 0x01);    /* drb.php:1268 */
    CHECK_INT(target_lookup("ZX81", "16K")->machine_id, 0x08);  /* drb.php:1275 */
    CHECK_INT(target_lookup("MSX2", "12_8")->machine_id, 0x0F); /* drb.php:1279 */
    CHECK_INT(target_lookup("C64", NULL)->machine_id, 0x02);    /* drb.php:1269 */
    CHECK_INT(target_lookup("CPC", NULL)->machine_id, 0x03);    /* drb.php:1270 */
    CHECK_INT(target_lookup("MSX", NULL)->machine_id, 0x04);    /* drb.php:1271 */
    CHECK_INT(target_lookup("ST", NULL)->machine_id, 0x05);     /* drb.php:1272 */
    CHECK_INT(target_lookup("AMIGA", NULL)->machine_id, 0x06);  /* drb.php:1273 */
    CHECK_INT(target_lookup("PCW", NULL)->machine_id, 0x07);    /* drb.php:1274 */
    CHECK_INT(target_lookup("CPM", NULL)->machine_id, 0x0B);    /* drb.php:1276 */
    CHECK_INT(target_lookup("CP4", NULL)->machine_id, 0x0E);    /* drb.php:1278 */
}

/* Submachine (drb.php:1249-1262; golden MSX2_12_8 byte2=0x87=135). */
TEST(submachine_id_msx2_and_default)
{
    CHECK_INT(target_lookup("MSX2", "12_8")->submachine_id, 135);
    CHECK_INT(target_lookup("MSX2", "5_6")->submachine_id, 0);
    CHECK_INT(target_lookup("MSX2", "5_8")->submachine_id, 128);
    CHECK_INT(target_lookup("MSX2", "6_6")->submachine_id, 1);
    CHECK_INT(target_lookup("MSX2", "6_8")->submachine_id, 129);
    CHECK_INT(target_lookup("MSX2", "7_6")->submachine_id, 2);
    CHECK_INT(target_lookup("MSX2", "7_8")->submachine_id, 130);
    CHECK_INT(target_lookup("MSX2", "8_6")->submachine_id, 3);
    CHECK_INT(target_lookup("MSX2", "8_8")->submachine_id, 131);
    CHECK_INT(target_lookup("MSX2", "10_6")->submachine_id, 5);
    CHECK_INT(target_lookup("MSX2", "10_8")->submachine_id, 133);
    CHECK_INT(target_lookup("MSX2", "12_6")->submachine_id, 7);
    CHECK_INT(target_lookup("ST", NULL)->submachine_id, 95);   /* drb.php:1261 default */
}

/* Base addresses (drb.php:1287-1299). */
TEST(base_addresses)
{
    CHECK_INT(target_lookup("ZX", "PLUS3")->base_address, 0x8400);
    CHECK_INT(target_lookup("ZX", "48K")->base_address, 0x8400);
    CHECK_INT(target_lookup("ZX81", "16K")->base_address, 0x0000);
    CHECK_INT(target_lookup("ZX81", "SD81B")->base_address, 0x8400);
    CHECK_INT(target_lookup("CP4", NULL)->base_address, 0x7080);
    CHECK_INT(target_lookup("MSX", NULL)->base_address, 0x0100);
    CHECK_INT(target_lookup("CPC", NULL)->base_address, 0x2880);
    CHECK_INT(target_lookup("PCW", NULL)->base_address, 0x0100);
    CHECK_INT(target_lookup("CPM", NULL)->base_address, 0x2000);
    CHECK_INT(target_lookup("C64", NULL)->base_address, 0x3880);
    /* drb.php:1301 default arm - HTML/AMIGA/ST/MSX2/PC fall through. */
    CHECK_INT(target_lookup("HTML", NULL)->base_address, 0x0000);
    CHECK_INT(target_lookup("AMIGA", NULL)->base_address, 0x0000);
    CHECK_INT(target_lookup("ST", NULL)->base_address, 0x0000);
    CHECK_INT(target_lookup("MSX2", "5_6")->base_address, 0x0000);
    CHECK_INT(target_lookup("PC", "VGA256")->base_address, 0x0000);
}

/* Byte order and padding (drb.php:1305-1313). */
TEST(byte_order_and_padding)
{
    CHECK_INT(target_lookup("ST", NULL)->big_endian, 1);
    CHECK_INT(target_lookup("AMIGA", NULL)->big_endian, 1);
    CHECK_INT(target_lookup("HTML", NULL)->big_endian, 0);
    CHECK_INT(target_lookup("HTML", NULL)->padding_platform, 1);
    CHECK_INT(target_lookup("PC", "TEXT")->padding_platform, 1);
    CHECK_INT(target_lookup("ST", NULL)->padding_platform, 1);
    CHECK_INT(target_lookup("AMIGA", NULL)->padding_platform, 1);
    CHECK_INT(target_lookup("CPC", NULL)->padding_platform, 0);
    CHECK_INT(target_lookup("ZX", "48K")->padding_platform, 0);
    CHECK_INT(target_lookup("MSX2", "5_6")->padding_platform, 0);
}

/* Durations (drb.php:1532-1571, DEFAULT_NOTE_DURATION=200 at 1530). */
TEST(durations)
{
    CHECK_INT(target_lookup("NEXTDAAD", NULL)->duration_base_length, 100);
    CHECK_INT(target_lookup("ZX", "48K")->duration_base_length, 195);
    CHECK_INT(target_lookup("ZX", "ESXDOS")->duration_base_length, 195);
    CHECK_INT(target_lookup("ZX", "128K")->duration_base_length, 100);
    CHECK_INT(target_lookup("ZX", "PLUS3")->duration_base_length, 100);
    CHECK_INT(target_lookup("ZX", "NEXT")->duration_base_length, 100);
    CHECK_INT(target_lookup("ZX", "UNO")->duration_base_length, 100);
    CHECK_INT(target_lookup("ZX81", "16K")->duration_base_length, 195);
    CHECK_INT(target_lookup("C64", NULL)->duration_base_length, 120);
    CHECK_INT(target_lookup("CP4", NULL)->duration_base_length, 80);
    CHECK_INT(target_lookup("PC", "VGA256")->duration_base_length, 120);
    CHECK_INT(target_lookup("PC", "CGA")->duration_base_length, 200);
    CHECK_INT(target_lookup("HTML", NULL)->duration_base_length, 230);
    CHECK_INT(target_lookup("CPC", NULL)->duration_base_length, 300);
    CHECK_INT(target_lookup("MSX", NULL)->duration_base_length, 230);
    CHECK_INT(target_lookup("MSX2", "5_6")->duration_base_length, 230);
    CHECK_INT(target_lookup("ST", NULL)->duration_base_length, 200);   /* default arm */
    CHECK_INT(target_lookup("AMIGA", NULL)->duration_base_length, 200); /* default arm */
    CHECK_INT(target_lookup("PCW", NULL)->duration_base_length, 200);   /* default arm */
    CHECK_INT(target_lookup("CPM", NULL)->duration_base_length, 200);   /* default arm */
}

/* BEEP swap and debug (drb.php:906-911, 1802). */
TEST(beep_swap_and_debug)
{
    CHECK_INT(target_lookup("ZX81", "16K")->beep_swap, 1);
    CHECK_INT(target_lookup("ZX", "48K")->beep_swap, 1);
    CHECK_INT(target_lookup("NEXTDAAD", NULL)->beep_swap, 1);
    CHECK_INT(target_lookup("C64", NULL)->beep_swap, 0);
    CHECK_INT(target_lookup("CPC", NULL)->beep_swap, 0);
    CHECK_INT(target_lookup("CPC", NULL)->debug_allowed, 1);
    CHECK_INT(target_lookup("ZX", "48K")->debug_allowed, 1);
    CHECK_INT(target_lookup("NEXTDAAD", NULL)->debug_allowed, 1);
    CHECK_INT(target_lookup("ST", NULL)->debug_allowed, 0);
    CHECK_INT(target_lookup("ZX81", "16K")->debug_allowed, 0);
}

/* XMessage container (drb.php:419-446). */
TEST(xmessage_size)
{
    CHECK_INT(target_lookup("ZX", "PLUS3")->xmessage_size_k, 16);
    CHECK_INT(target_lookup("ZX", "128K")->xmessage_size_k, 16);
    CHECK_INT(target_lookup("ZX", "48K")->xmessage_size_k, 64);
    CHECK_INT(target_lookup("MSX2", "8_6")->xmessage_size_k, 16);
    CHECK_INT(target_lookup("CPC", NULL)->xmessage_size_k, 2);
    CHECK_INT(target_lookup("C64", NULL)->xmessage_size_k, 2);
    CHECK_INT(target_lookup("CP4", NULL)->xmessage_size_k, 2);
    CHECK_INT(target_lookup("PC", "VGA256")->xmessage_size_k, 64);
    CHECK_INT(target_lookup("PC", "EGA")->xmessage_size_k, 0);
    CHECK_INT(target_lookup("ZX81", "16K")->xmessage_size_k, 64);
    CHECK_INT(target_lookup("MSX", NULL)->xmessage_size_k, 64);
    CHECK_INT(target_lookup("PCW", NULL)->xmessage_size_k, 64);
    CHECK_INT(target_lookup("CPM", NULL)->xmessage_size_k, 64);
    CHECK_INT(target_lookup("HTML", NULL)->xmessage_size_k, 64);
    CHECK_INT(target_lookup("AMIGA", NULL)->xmessage_size_k, 64);
    CHECK_INT(target_lookup("ST", NULL)->xmessage_size_k, 64);
}

/* Pitch adjustment (drb.php:1578-1591 getPitchAdjustment). The MSX1 arm
   (drb.php:1587, -12) matches no valid target name and is dead code -
   MSX itself falls to the default (0). */
TEST(pitch_adjustment)
{
    CHECK_INT(target_lookup("C64", NULL)->pitch_adjustment, -12);
    CHECK_INT(target_lookup("ZX", "ESXDOS")->pitch_adjustment, -24);
    CHECK_INT(target_lookup("NEXTDAAD", NULL)->pitch_adjustment, -24);
    CHECK_INT(target_lookup("PC", "TEXT")->pitch_adjustment, -24);
    CHECK_INT(target_lookup("HTML", NULL)->pitch_adjustment, -24);
    CHECK_INT(target_lookup("MSX", NULL)->pitch_adjustment, 0);   /* MSX1 arm is dead - drb.php:1587 */
    CHECK_INT(target_lookup("CPC", NULL)->pitch_adjustment, 0);
    CHECK_INT(target_lookup("ZX81", "SD81B")->pitch_adjustment, 0);
}

/* Lookup contract (brief interfaces section + drb.php:1718). */
TEST(lookup_contract)
{
    CHECK(target_lookup("ZX", NULL) == NULL);        /* ZX requires a subtarget row */
    CHECK(target_lookup("ZX", "") == NULL);
    CHECK(target_lookup("NEXTDAAD", "") != NULL);     /* "" == NULL for bare targets */
    CHECK(target_lookup("nextdaad", NULL) != NULL);   /* case-insensitive */
    CHECK(target_lookup("zx", "48k") != NULL);        /* case-insensitive both */
    CHECK(target_lookup("", NULL) == NULL);
    CHECK(target_lookup("SPECTRUM", NULL) == NULL);   /* unknown name */
    CHECK(target_lookup("MSX2", "9_6") == NULL);      /* unknown subtarget */

    CHECK_INT(target_takes_subtarget("PC"), 1);
    CHECK_INT(target_takes_subtarget("ZX"), 1);
    CHECK_INT(target_takes_subtarget("ZX81"), 1);
    CHECK_INT(target_takes_subtarget("MSX2"), 1);
    CHECK_INT(target_takes_subtarget("CPM"), 0);
    CHECK_INT(target_takes_subtarget("NEXTDAAD"), 0);
    CHECK_INT(target_takes_subtarget("nosuchtarget"), 0);

    CHECK_INT(target_name_valid("msx2"), 1);
    CHECK_INT(target_name_valid("NEXTDAAD"), 1);
    CHECK_INT(target_name_valid("SPECTRUM"), 0);
    CHECK_INT(target_name_valid(""), 0);
}

int main(void)
{
    RUN(row_count_is_35);
    RUN(nextdaad_row_matches_reference);
    RUN(machine_id_traps);
    RUN(submachine_id_msx2_and_default);
    RUN(base_addresses);
    RUN(byte_order_and_padding);
    RUN(durations);
    RUN(beep_swap_and_debug);
    RUN(xmessage_size);
    RUN(pitch_adjustment);
    RUN(lookup_contract);
    return test_summary("targets");
}
