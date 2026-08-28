/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/ctlextern.h - Copyright (C) 2026 Dan Gibson.

   PORT: UCTLExtern.pas (D:/DRC/src, branch nextdaad) - the list of
   binary files queued by #extern / #int / #sfx. Each entry is the
   single pipe-joined string `FileName|ExternType` (ExternType is the
   literal EXTERN, INT or SFX) - that composed string is the entire
   contract with the JSON `externs` array (analysis 22.5/16.7), so it
   is stored composed, exactly as AddCTL_Extern builds it, not as two
   fields. */
#ifndef NDRC_FRONT_CTLEXTERN_H
#define NDRC_FRONT_CTLEXTERN_H

#include <stddef.h>

#include "arena.h"

typedef struct CTLExternList CTLExternList;

/* PORT: the unit global `CTLExternList: TCTLExternList` starts as an
   empty dynamic array; instance-based here like the other front
   containers (sintactic.c owns the one instance, mirroring the Pascal
   global). */
CTLExternList *ctlextern_new(Arena *a);

/* PORT: AddCTL_Extern (UCTLExtern.pas:12-16) - appends
   `filename|extern_type`. No validation, no dedup, exactly as the
   Pascal (existence checking is ParseExtern's job, before this
   call). */
void ctlextern_add(CTLExternList *list, Arena *a, const char *filename,
                    const char *extern_type);

size_t ctlextern_count(const CTLExternList *list);

/* The i-th composed `FileName|TYPE` string, or NULL out of range. */
const char *ctlextern_at(const CTLExternList *list, size_t i);

#endif /* NDRC_FRONT_CTLEXTERN_H */
