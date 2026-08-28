/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/process.c - Copyright (C) 2026 Dan Gibson. */
#include "process.h"

void process_condacts_add(Vec_ProcessCondact *list, Arena *a, long opcode,
                           int num_params,
                           const CondactParam params[MAX_CONDACT_PARAMS],
                           int is_db)
{
    ProcessCondact *c = arena_alloc(a, sizeof(*c));
    int i;
    c->opcode = opcode;
    c->is_db = is_db;
    c->num_params = num_params;
    for (i = 0; i < MAX_CONDACT_PARAMS; i++) {
        c->params[i] = params[i];
    }
    vec_push_ProcessCondact(list, c);
}

struct ProcessTable {
    ProcessSlot *slots; /* arena-allocated, MAX_PROCESSES + 1 entries */
};

ProcessTable *processtable_new(Arena *a)
{
    ProcessTable *t = arena_alloc(a, sizeof(*t));
    long i;
    /* PORT: InitializeProcesses (UProcess.pas:33-41) - MAX_PROCESSES
       (255) + 1 = 256 slots, 0..MAX_PROCESSES inclusive. */
    t->slots = arena_alloc(a, sizeof(ProcessSlot) * (size_t)(MAX_PROCESSES + 1));
    for (i = 0; i <= MAX_PROCESSES; i++) {
        t->slots[i].value = i;
        t->slots[i].entries = vec_new_ProcessEntry(a);
    }
    return t;
}

size_t processtable_len(const ProcessTable *t)
{
    (void)t;
    return (size_t)(MAX_PROCESSES + 1);
}

const ProcessSlot *processtable_get(const ProcessTable *t, long process)
{
    if (process < 0 || process > MAX_PROCESSES) return NULL;
    return &t->slots[process];
}

ProcessSlot *processtable_at(ProcessTable *t, Arena *a, Diag *d,
                              long process)
{
    (void)a;
    if (process < 0 || process > MAX_PROCESSES) {
        diag_fatal(d,
                   "process number %ld out of range (maximum %d)",
                   process, MAX_PROCESSES);
        return NULL;
    }
    return &t->slots[process];
}

void process_add_entry(ProcessSlot *slot, Arena *a, long verb, long noun,
                        const char *skip_label, Vec_ProcessCondact *condacts)
{
    ProcessEntry *e = arena_alloc(a, sizeof(*e));
    e->verb = verb;
    e->noun = noun;
    e->skip_label = skip_label ? arena_strdup(a, skip_label) : NULL;
    e->condacts = condacts;
    vec_push_ProcessEntry(slot->entries, e);
}
