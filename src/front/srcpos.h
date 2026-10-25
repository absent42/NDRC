/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/srcpos.h - Copyright (C) 2026 Dan Gibson.

   Source positions the parser records for -srcmap: process numbers,
   entry verbs, condact rows and #define names. Positions are in the
   ORIGINAL file (temp-file line mapped through the include map);
   col is the 1-based START column of the token. Storage comes from
   the sintactic_init arena and lives as long as it does.

   ORDER CONTRACT: [proc][entry][cond] follows jsonexport.c's walk -
   process 0..last, each slot's entries, each entry's condact rows -
   so index n here is the nth object the JSON writes at that level. */
#ifndef NDRC_FRONT_SRCPOS_H
#define NDRC_FRONT_SRCPOS_H

#include "arena.h"

/* file is NULL for a position the include map could not resolve
   (no preparse ran) and for a process number never opened by /PRO. */
typedef struct SrcPos { const char *file; int line; int col; } SrcPos;

/* One entry's condact rows; synonym headers share one list. */
typedef struct SrcPosConds SrcPosConds;

/* ---- recording (sintactic.c) ---- */

void srcpos_reset(Arena *a);
/* Maps a temp-file line through the include map. */
SrcPos srcpos_at(long temp_line, int col);
/* The first /PRO for a number keeps its position. */
void srcpos_set_proc(long proc, SrcPos pos);
SrcPosConds *srcpos_conds_new(void);
void srcpos_conds_add(SrcPosConds *conds, SrcPos pos);
/* Appends one entry to `proc`; conds may be NULL (no rows). */
void srcpos_add_entry(long proc, SrcPos pos, SrcPosConds *conds);
void srcpos_add_define(const char *name, long value);

/* ---- reading (back end) ---- */

int srcpos_proc_count(void);
SrcPos srcpos_proc(int proc);
int srcpos_entry_count(int proc);
SrcPos srcpos_entry(int proc, int entry);
int srcpos_cond_count(int proc, int entry);
SrcPos srcpos_cond(int proc, int entry, int cond);
int srcpos_define_count(void);
const char *srcpos_define_name(int i);
long srcpos_define_value(int i);

#endif /* NDRC_FRONT_SRCPOS_H */
