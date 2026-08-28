/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/targets.c - per-machine configuration table.
   Copyright (C) 2026 Dan Gibson.

   35 rows: 14 target names (drb.php:1233-1236 isValidTarget); MSX2,
   PC, ZX, ZX81 split per subtarget (drb.php:1238-1247/1242-1246: MSX2
   x12, ZX x6, ZX81 x2, PC x5 = 25 rows), the other ten stay bare = 35.

   Column sources (per row below where a value is not the obvious
   default):
     machine_id           drb.php:1265-1281 getMachineIDByTarget
     submachine_id         drb.php:1249-1262 getSubMachineIDByTarget
     base_address          drb.php:1284-1303 getBaseAddressByTarget
     big_endian            drb.php:1310-1313 isLittleEndianPlatform
                            (ST/AMIGA only - see targets.h byte-order note)
     padding_platform      drb.php:1305-1308 isPaddingPlatform
     duration_base_length  drb.php:1532-1571 getBaseLength (ms);
                            DEFAULT_NOTE_DURATION=200 at drb.php:1530
     beep_swap             drb.php:906-911 (ZX/ZX81/NEXTDAAD only)
     debug_allowed         drb.php:1802 (ZX/CPC/NEXTDAAD only)
     xmessage_size_k       drb.php:419-446 getXMessageFileSizeByTarget
                            (0 = target unsupported for XMessages)
     pitch_adjustment      drb.php:1578-1591 getPitchAdjustment; C64 -12,
                            CP4 0, ZX -24, NEXTDAAD -24, PC -24, HTML -24,
                            default 0 (ZX81, CPC, MSX, MSX2, ST, AMIGA,
                            PCW, CPM). The MSX1 arm (drb.php:1587, -12)
                            matches no valid target name and is dead code -
                            MSX itself falls to the default.

   Traps confirmed against the PHP while transcribing:
     - getMachineIDByTarget's PC arm (drb.php:1267) returns 0x00 on the
       function's FIRST line, unconditionally - so PC+VGA256 never
       reaches the later "PC && VGA256 -> 0x0D" line (drb.php:1277);
       only bare HTML reaches it. PC is 0x00 for every subtarget.
     - getBaseAddressByTarget's switch has no case for HTML, AMIGA, ST,
       MSX2 or PC, so all five fall to `default: return 0` (drb.php:1301).
     - getXMessageFileSizeByTarget's PC arm (drb.php:443-444) has no
       break before `default: return 0;`, so PC falls through to 0 for
       every subtarget except VGA256, which returns 64 first.
     - getBaseLength's MSX case has no break, so MSX and MSX2 share the
       230 result (drb.php:1566-1567 `case 'MSX': case 'MSX2': ...`). */
#include "targets.h"
#include "str.h"

static const Target targets[] = {
    /* name       subtarget  mach  subm  base    BE pad dur  swap dbg xmsgK pitch */
    { "NEXTDAAD", NULL,      0x0C,  95, 0x0000,  0, 0, 100, 1, 1, 64, -24 },

    /* ZX: base 0x8400 for every subtarget (drb.php:1290, no per-sub
       branch); beep_swap and debug_allowed both 1 for all of ZX. */
    { "ZX", "48K",    0x01, 95, 0x8400, 0, 0, 195, 1, 1, 64, -24 },
    { "ZX", "128K",   0x01, 95, 0x8400, 0, 0, 100, 1, 1, 16, -24 },
    { "ZX", "PLUS3",  0x01, 95, 0x8400, 0, 0, 100, 1, 1, 16, -24 },
    { "ZX", "ESXDOS", 0x01, 95, 0x8400, 0, 0, 195, 1, 1, 64, -24 },
    { "ZX", "NEXT",   0x01, 95, 0x8400, 0, 0, 100, 1, 1, 64, -24 },
    { "ZX", "UNO",    0x01, 95, 0x8400, 0, 0, 100, 1, 1, 64, -24 },

    /* ZX81: base differs by subtarget (drb.php:1298-1300); debug not
       allowed (not in the drb.php:1802 exception list). ZX81 is not
       named in getPitchAdjustment's switch, so pitch is the default 0. */
    { "ZX81", "16K",   0x08, 95, 0x0000, 0, 0, 195, 1, 0, 64, 0 },
    { "ZX81", "SD81B", 0x08, 95, 0x8400, 0, 0, 195, 1, 0, 64, 0 },

    /* PC: machine_id 0x00 always (see trap note above); padding
       platform; xmessage_size_k 0 except VGA256; pitch -24 for every
       subtarget (drb.php:1586). */
    { "PC", "VGA256", 0x00, 95, 0x0000, 0, 1, 120, 0, 0, 64, -24 },
    { "PC", "VGA",    0x00, 95, 0x0000, 0, 1, 200, 0, 0, 0, -24 },
    { "PC", "CGA",    0x00, 95, 0x0000, 0, 1, 200, 0, 0, 0, -24 },
    { "PC", "EGA",    0x00, 95, 0x0000, 0, 1, 200, 0, 0, 0, -24 },
    { "PC", "TEXT",   0x00, 95, 0x0000, 0, 1, 200, 0, 0, 0, -24 },

    /* MSX2: submachine_id = (mode - 5) + (128 if char width is 8)
       (drb.php:1251-1258); machine_id 0x0F; duration 230 (shares the
       MSX case, drb.php:1566-1567); xmessage 16K for every subtarget.
       MSX2 is not named in getPitchAdjustment's switch (only the dead
       MSX1 arm is), so pitch is the default 0. */
    { "MSX2", "5_6",  0x0F,   0, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "5_8",  0x0F, 128, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "6_6",  0x0F,   1, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "6_8",  0x0F, 129, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "7_6",  0x0F,   2, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "7_8",  0x0F, 130, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "8_6",  0x0F,   3, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "8_8",  0x0F, 131, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "10_6", 0x0F,   5, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "10_8", 0x0F, 133, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "12_6", 0x0F,   7, 0x0000, 0, 0, 230, 0, 0, 16, 0 },
    { "MSX2", "12_8", 0x0F, 135, 0x0000, 0, 0, 230, 0, 0, 16, 0 },

    /* Remaining bare (no subtarget) targets. Pitch: C64 -12 (drb.php:1582),
       CP4 0 (drb.php:1583), HTML -24 (drb.php:1588); CPC, CPM, MSX, PCW,
       AMIGA and ST are not named in getPitchAdjustment's switch (MSX's
       only near-match, MSX1, is dead code - drb.php:1587) so all six
       fall to the default 0 (drb.php:1589). */
    { "C64",   NULL, 0x02, 95, 0x3880, 0, 0, 120, 0, 0, 2, -12 },
    { "CPC",   NULL, 0x03, 95, 0x2880, 0, 0, 300, 0, 1, 2, 0 },
    { "CP4",   NULL, 0x0E, 95, 0x7080, 0, 0,  80, 0, 0, 2, 0 },
    { "CPM",   NULL, 0x0B, 95, 0x2000, 0, 0, 200, 0, 0, 64, 0 },
    { "MSX",   NULL, 0x04, 95, 0x0100, 0, 0, 230, 0, 0, 64, 0 },
    { "PCW",   NULL, 0x07, 95, 0x0100, 0, 0, 200, 0, 0, 64, 0 },
    { "AMIGA", NULL, 0x06, 95, 0x0000, 1, 1, 200, 0, 0, 64, 0 },
    { "ST",    NULL, 0x05, 95, 0x0000, 1, 1, 200, 0, 0, 64, 0 },
    { "HTML",  NULL, 0x0D, 95, 0x0000, 0, 1, 230, 0, 0, 64, -24 },
};

#define TARGET_COUNT ((int)(sizeof(targets) / sizeof(targets[0])))

/* NULL and "" are both "no subtarget" (brief interface note). */
static int subtarget_is_empty(const char *s)
{
    return s == NULL || s[0] == '\0';
}

const Target *target_lookup(const char *name, const char *subtarget)
{
    int i;
    if (name == NULL) return NULL;
    for (i = 0; i < TARGET_COUNT; i++) {
        const Target *t = &targets[i];
        if (!str_ieq(t->name, name)) continue;
        if (t->subtarget == NULL) {
            if (subtarget_is_empty(subtarget)) return t;
        } else {
            if (!subtarget_is_empty(subtarget) && str_ieq(t->subtarget, subtarget)) {
                return t;
            }
        }
    }
    return NULL;
}

int target_name_valid(const char *name)
{
    int i;
    if (name == NULL) return 0;
    for (i = 0; i < TARGET_COUNT; i++) {
        if (str_ieq(targets[i].name, name)) return 1;
    }
    return 0;
}

int target_takes_subtarget(const char *name)
{
    int i;
    if (name == NULL) return 0;
    for (i = 0; i < TARGET_COUNT; i++) {
        if (str_ieq(targets[i].name, name) && targets[i].subtarget != NULL) {
            return 1;
        }
    }
    return 0;
}
