/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/process.h - Copyright (C) 2026 Dan Gibson.

   PORT: UProcess.pas + UProcessCondactList.pas (D:/DRC/src, branch
   nextdaad) - the process table, its entries
   (verb/noun pairs), and each entry's condact list. Folded into one
   file (per docs/dev/phase2a-design.md's architecture table): the two
   Pascal units are a tight parent/child pair (TProcessEntryList.
   Condacts is a TPProcessCondactList) with no other consumer of
   UProcessCondactList.pas anywhere in the port. */
#ifndef NDRC_FRONT_PROCESS_H
#define NDRC_FRONT_PROCESS_H

#include <stddef.h>

#include "arena.h"
#include "diag.h"
#include "vec.h"

#include "constants.h"

/* PORT: TParam (UProcessCondactList.pas:10-13). */
typedef struct CondactParam {
    long value;
    int indirection;
} CondactParam;

/* PORT: TProcessCondactList's data fields (UProcessCondactList.pas:
   20-26), `Next` omitted (Vec-backed, append-only). `params` is always
   a fixed MAX_CONDACT_PARAMS(3)-slot array regardless of `num_params` -
   AddProcessCondact copies all 3 slots verbatim every time. `is_db`
   flags a #DB/#DW/#HEX/#INCBIN-emitted byte for JSON reporting. */
typedef struct ProcessCondact {
    long opcode;
    int is_db;
    int num_params;
    CondactParam params[MAX_CONDACT_PARAMS];
} ProcessCondact;

VEC_DECLARE(ProcessCondact, ProcessCondact *)

/* PORT: AddProcessCondact (UProcessCondactList.pas:33-51). Tail-append
   (here: vec_push), copying all MAX_CONDACT_PARAMS slots verbatim
   regardless of `num_params`, exactly as the Pascal does. `list` must
   already exist (vec_new_ProcessCondact); this mirrors the Pascal's
   VAR-parameter append onto a list a caller already owns per entry. */
void process_condacts_add(Vec_ProcessCondact *list, Arena *a, long opcode,
                           int num_params,
                           const CondactParam params[MAX_CONDACT_PARAMS],
                           int is_db);

/* PORT: TProcessEntryList's data fields (UProcess.pas:12-17), `Next`
   omitted (Vec-backed, append-only). `skip_label` mirrors
   TProcessEntryList.SkipLabel (UProcess.pas:14), declared but never
   assigned by anything in the units this port covers - ported for
   record-shape fidelity; expect it to stay NULL/empty on every
   traversed path. */
typedef struct ProcessEntry {
    long verb, noun;
    const char *skip_label;
    Vec_ProcessCondact *condacts;
} ProcessEntry;

VEC_DECLARE(ProcessEntry, ProcessEntry *)

/* PORT: TProcess's data fields (UProcess.pas:19-22). `value` is
   pre-filled to the slot's own index by processtable_new, exactly as
   InitializeProcesses does (UProcess.pas:33-41: `Processes[i].Value :=
   i`). */
typedef struct ProcessSlot {
    long value;
    Vec_ProcessEntry *entries;
} ProcessSlot;

typedef struct ProcessTable ProcessTable;

/* PORT: InitializeProcesses (UProcess.pas:33-41). Pre-fills every
   slot 0..MAX_PROCESSES INCLUSIVE - MAX_PROCESSES is 255, so this is
   256 slots, one more than the constant name suggests (Processes:
   ARRAY[0..MAX_PROCESSES], UProcess.pas:24). */
ProcessTable *processtable_new(Arena *a);

/* Read-only iteration surface (Task 8's JSON walk): 0..MAX_PROCESSES
   inclusive are always valid immediately after processtable_new (every
   slot was pre-filled). Returns NULL for anything outside that range;
   no diagnostic, no Arena - a pure query. */
size_t processtable_len(const ProcessTable *t); /* MAX_PROCESSES + 1 = 256 */
const ProcessSlot *processtable_get(const ProcessTable *t, long process);

/* The reference relies on FPC {$R+} RTE 201 for process > 255; in C
   that is a buffer overrun, so out-of-range FATALs and returns NULL
   (already reported - caller stops, same contract as labels_add's
   -2). */
ProcessSlot *processtable_at(ProcessTable *t, Arena *a, Diag *d,
                              long process);

/* PORT: AddProcessEntry (UProcess.pas:43-54). Tail-append. */
void process_add_entry(ProcessSlot *slot, Arena *a, long verb, long noun,
                        const char *skip_label, Vec_ProcessCondact *condacts);

#endif /* NDRC_FRONT_PROCESS_H */
