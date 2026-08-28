/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/labels.c - Copyright (C) 2026 Dan Gibson. */
#include "labels.h"

#include <string.h>

#include "constants.h"

struct LabelTable {
    LabelData *slots; /* arena-allocated, MAX_LABELS entries */
    size_t count;      /* NextFreeLabelSlot */
};

LabelTable *labels_new(Arena *a)
{
    LabelTable *t = arena_alloc(a, sizeof(*t));
    t->slots = arena_alloc(a, sizeof(LabelData) * (size_t)MAX_LABELS);
    t->count = 0;
    return t;
}

long labels_add(LabelTable *t, Arena *a, Diag *d, const char *label,
                 long process, long entry, int is_forward, int condact)
{
    size_t i;

    for (i = 0; i < t->count; i++) {
        LabelData *e = &t->slots[i];
        if (strcmp(e->skip_label, label) != 0) continue;

        if (e->is_forward && !is_forward) {
            e->process = process;
            e->entry = entry;
            e->is_forward = 0;
            e->condact = condact;
            return (long)i;
        }
        if (e->is_forward && is_forward) {
            return (long)i;
        }
        return -1; /* repeated non-forward declaration */
    }

    if (t->count >= (size_t)MAX_LABELS) {
        diag_fatal(d,
                    "label table full: %d labels already defined "
                    "(maximum %d) while adding \"%s\"",
                    (int)t->count, MAX_LABELS, label);
        return -2;
    }

    t->slots[t->count].skip_label = arena_strdup(a, label);
    t->slots[t->count].process = process;
    t->slots[t->count].entry = entry;
    t->slots[t->count].is_forward = is_forward;
    t->slots[t->count].condact = condact;
    return (long)(t->count++);
}

int labels_find(const LabelTable *t, const char *label, LabelData *out)
{
    size_t i;
    int found = 0;

    for (i = 0; i < t->count; i++) {
        const LabelData *e = &t->slots[i];
        if (e->is_forward) continue;
        if (strcmp(e->skip_label, label) != 0) continue;
        *out = *e;
        found = 1; /* keep scanning: GetLabelData keeps the LAST match */
    }
    return found;
}

size_t labels_count(const LabelTable *t)
{
    return t->count;
}

const LabelData *labels_at(const LabelTable *t, size_t index)
{
    if (index >= t->count) return NULL;
    return &t->slots[index];
}
