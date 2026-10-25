/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/srcpos.c - Copyright (C) 2026 Dan Gibson. */
#include "srcpos.h"

#include <string.h>

#include "vec.h"

#include "constants.h"
#include "include.h"

struct SrcPosConds {
    SrcPos *v;
    size_t len, cap;
};

typedef struct SrcEntry {
    SrcPos pos;
    SrcPosConds *conds;
} SrcEntry;

typedef struct SrcDefine {
    const char *name;
    long value;
} SrcDefine;

VEC_DECLARE(SrcEntry, SrcEntry *)
VEC_DECLARE(SrcDefine, SrcDefine *)

typedef struct SrcProc {
    SrcPos pos;
    int opened;
    Vec_SrcEntry *entries;
} SrcProc;

static Arena *g_arena;
static SrcProc g_procs[MAX_PROCESSES + 1];
static int g_proc_count;
static Vec_SrcDefine *g_defines;

static const SrcPos g_none = { NULL, 0, 0 };

void srcpos_reset(Arena *a)
{
    g_arena = a;
    memset(g_procs, 0, sizeof g_procs);
    g_proc_count = 0;
    g_defines = vec_new_SrcDefine(a);
}

SrcPos srcpos_at(long temp_line, int col)
{
    IncludeData inc;
    SrcPos p;
    p.col = col;
    if (include_get_data(temp_line, &inc)) {
        p.file = inc.original_filename;
        p.line = (int)inc.original_line;
    } else {
        p.file = NULL;
        p.line = (int)temp_line;
    }
    return p;
}

void srcpos_set_proc(long proc, SrcPos pos)
{
    if (proc < 0 || proc > MAX_PROCESSES) return;
    if (!g_procs[proc].opened) {
        g_procs[proc].pos = pos;
        g_procs[proc].opened = 1;
    }
    if (proc + 1 > g_proc_count) g_proc_count = (int)proc + 1;
}

SrcPosConds *srcpos_conds_new(void)
{
    SrcPosConds *c = arena_alloc(g_arena, sizeof(*c));
    c->v = NULL;
    c->len = c->cap = 0;
    return c;
}

void srcpos_conds_add(SrcPosConds *c, SrcPos pos)
{
    if (c->len == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        SrcPos *grown = arena_alloc(g_arena, sizeof(SrcPos) * cap);
        if (c->len) memcpy(grown, c->v, sizeof(SrcPos) * c->len);
        c->v = grown;
        c->cap = cap;
    }
    c->v[c->len++] = pos;
}

void srcpos_add_entry(long proc, SrcPos pos, SrcPosConds *conds)
{
    SrcEntry *e;
    if (proc < 0 || proc > MAX_PROCESSES) return;
    if (g_procs[proc].entries == NULL)
        g_procs[proc].entries = vec_new_SrcEntry(g_arena);
    e = arena_alloc(g_arena, sizeof(*e));
    e->pos = pos;
    e->conds = conds;
    vec_push_SrcEntry(g_procs[proc].entries, e);
}

void srcpos_add_define(const char *name, long value)
{
    SrcDefine *d = arena_alloc(g_arena, sizeof(*d));
    d->name = name;
    d->value = value;
    vec_push_SrcDefine(g_defines, d);
}

int srcpos_proc_count(void)
{
    return g_proc_count;
}

SrcPos srcpos_proc(int proc)
{
    if (proc < 0 || proc >= g_proc_count) return g_none;
    return g_procs[proc].pos;
}

static const SrcEntry *entry_at(int proc, int entry)
{
    if (proc < 0 || proc >= g_proc_count || entry < 0) return NULL;
    if (g_procs[proc].entries == NULL) return NULL;
    return vec_at_SrcEntry(g_procs[proc].entries, (size_t)entry);
}

int srcpos_entry_count(int proc)
{
    if (proc < 0 || proc >= g_proc_count) return 0;
    if (g_procs[proc].entries == NULL) return 0;
    return (int)vec_len_SrcEntry(g_procs[proc].entries);
}

SrcPos srcpos_entry(int proc, int entry)
{
    const SrcEntry *e = entry_at(proc, entry);
    return e ? e->pos : g_none;
}

int srcpos_cond_count(int proc, int entry)
{
    const SrcEntry *e = entry_at(proc, entry);
    return (e && e->conds) ? (int)e->conds->len : 0;
}

SrcPos srcpos_cond(int proc, int entry, int cond)
{
    const SrcEntry *e = entry_at(proc, entry);
    if (e == NULL || e->conds == NULL || cond < 0 ||
        (size_t)cond >= e->conds->len)
        return g_none;
    return e->conds->v[cond];
}

int srcpos_define_count(void)
{
    return g_defines ? (int)vec_len_SrcDefine(g_defines) : 0;
}

const char *srcpos_define_name(int i)
{
    const SrcDefine *d;
    if (g_defines == NULL || i < 0) return NULL;
    d = vec_at_SrcDefine(g_defines, (size_t)i);
    return d ? d->name : NULL;
}

long srcpos_define_value(int i)
{
    const SrcDefine *d;
    if (g_defines == NULL || i < 0) return 0;
    d = vec_at_SrcDefine(g_defines, (size_t)i);
    return d ? d->value : 0;
}
