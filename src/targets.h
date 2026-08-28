/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/targets.h - per-machine configuration table.
   Copyright (C) 2026 Dan Gibson.

   The targets table centralizes per-target facts so they live in one place
   (design S5.1): machine and submachine IDs, base address, byte order, and
   padding conventions. This avoids scattering target-specific values and
   decisions throughout the compiler.

   Note on byte order naming (analysis S24): DRC's isLittleEndianPlatform
   parameter name inverts the meaning of its value (the flag indicates the
   file's byte order, not the platform's). This table uses the file's real
   byte order in its field name to avoid confusion, as every code path
   depends directly on the data format, not on the host platform.

   The ZX Spectrum is not the 0x00 default (analysis S7): all future rows
   must be added from the corrected table only; do not infer values from
   the pattern of early entries.

   Some targets (MSX2, PC, ZX, ZX81 - drb.php:1238-1247 isValidSubtarget)
   are split across several rows, one per subtarget; the rest have a
   single bare row (subtarget field NULL). */
#ifndef NDRC_TARGETS_H
#define NDRC_TARGETS_H

typedef struct Target {
    const char *name;              /* canonical upper-case target name */
    const char *subtarget;         /* canonical upper-case subtarget, NULL when none */
    unsigned machine_id;           /* drb.php:1265-1281 */
    unsigned submachine_id;        /* drb.php:1249-1262 */
    unsigned base_address;         /* drb.php:1287-1299 */
    int big_endian;                /* drb.php:1310: ST and AMIGA only */
    int padding_platform;          /* drb.php:1305: PC, ST, AMIGA, HTML */
    unsigned duration_base_length; /* drb.php:1532-1571, milliseconds */
    int beep_swap;                 /* drb.php:906-911: ZX, ZX81, NEXTDAAD */
    int debug_allowed;             /* drb.php:1802: ZX, CPC, NEXTDAAD */
    unsigned xmessage_size_k;      /* drb.php:419-446, 0 = unsupported */
    int pitch_adjustment;          /* drb.php:1578-1591 getPitchAdjustment;
                                       the MSX1 arm (drb.php:1587, -12) matches
                                       no valid target name and is dead - MSX
                                       falls to the default (0) */
} Target;

/* subtarget: NULL or "" for targets without one. Exact match on both
   (case-insensitive via str_ieq, as today). NULL if no such row. */
const Target *target_lookup(const char *name, const char *subtarget);
/* 1 iff any row has this name (case-insensitive). */
int target_name_valid(const char *name);
/* 1 iff this target consumes a subtarget argument - drb.php:1718:
   MSX2, PC, ZX, ZX81. Derived from the rows (any row with a non-NULL
   subtarget), not a second hand-written list. */
int target_takes_subtarget(const char *name);

#endif /* NDRC_TARGETS_H */
