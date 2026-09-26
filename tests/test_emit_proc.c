/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_emit_proc.c - Copyright (C) 2026 Dan Gibson.

   Entry sharing in emit_processes: entries share one compiled body only
   when their bytecode is identical. Indirection on either parameter
   changes the bytecode, so it must keep entries apart. Drives
   emit_processes on a minimal NEXTDAAD adventure (no padding, base 0),
   where the output is the condact bytecode alone. */
#include "test.h"
#include "arena.h"
#include "back/emit.h"
#include "diag.h"
#include "model.h"
#include "str.h"
#include "targets.h"

#include <string.h>

#define MES_OP   77
#define LET_OP   51
#define DONE_OP  22
#define INDIR_OP 122

static Condact *condact(Arena *a, long opcode, const char *name, long np,
                        long p1, long ind1, long p2, long ind2)
{
    Condact *c = arena_calloc(a, sizeof(*c));
    c->Opcode = opcode;
    c->Condact = name;
    c->NumParams = np;
    c->Param1 = p1;
    c->Indirection1 = ind1;
    c->Param2 = p2;
    c->Indirection2 = ind2;
    return c;
}

/* Appends `<c> DONE` as a new entry of process p. */
static void add_entry(Arena *a, Process *p, Condact *c)
{
    ProcEntry *e = arena_calloc(a, sizeof(*e));
    e->Entry = "_";
    e->Verb = 255;
    e->Noun = 255;
    e->condacts = vec_new_Condact(a);
    vec_push_Condact(e->condacts, c);
    vec_push_Condact(e->condacts, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    vec_push_ProcEntry(p->entries, e);
}

static Process *new_adventure(Arena *a, Adventure *adv)
{
    Process *p = arena_calloc(a, sizeof(*p));
    memset(adv, 0, sizeof(*adv));
    adv->v3code = 1;
    adv->processes = vec_new_Process(a);
    adv->other_strings = vec_new_Message(a);
    adv->xmessages = vec_new_Message(a);
    p->entries = vec_new_ProcEntry(a);
    vec_push_Process(adv->processes, p);
    return p;
}

static int count(const Str *out, const unsigned char *pat, size_t n)
{
    const unsigned char *b = (const unsigned char *)str_bytes(out);
    size_t len = str_len(out), i;
    int hits = 0;

    for (i = 0; i + n <= len; i++) {
        if (memcmp(b + i, pat, n) == 0) hits++;
    }
    return hits;
}

static void emit(Arena *a, Adventure *adv, Str *out)
{
    Diag *d = diag_new(a);
    const Target *t = target_lookup("NEXTDAAD", NULL);
    long addr = 0;

    CHECK(t != NULL);
    if (t == NULL) return;
    emit_processes(out, &addr, d, t, adv, 0);
}

/* MES 0 / MES @0: the second must get its own `MES|0x80 0 DONE` body. */
TEST(first_param_indirection_is_not_shared)
{
    static const unsigned char direct[]   = { MES_OP, 0, DONE_OP };
    static const unsigned char indirect[] = { MES_OP | 0x80, 0, DONE_OP };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);

    add_entry(a, p, condact(a, MES_OP, "MES", 1, 0, 0, 0, 0));
    add_entry(a, p, condact(a, MES_OP, "MES", 1, 0, 1, 0, 0));
    emit(a, &adv, out);

    CHECK_INT(count(out, direct, sizeof direct), 1);
    CHECK_INT(count(out, indirect, sizeof indirect), 1);
    arena_free(a);
}

/* LET 100 200 / LET 100 @200: the second must keep its INDIR prefix. */
TEST(second_param_indirection_is_not_shared)
{
    static const unsigned char direct[]   = { LET_OP, 100, 200, DONE_OP };
    static const unsigned char indirect[] = { INDIR_OP, 200, LET_OP, 100, 200, DONE_OP };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);

    add_entry(a, p, condact(a, LET_OP, "LET", 2, 100, 0, 200, 0));
    add_entry(a, p, condact(a, LET_OP, "LET", 2, 100, 0, 200, 1));
    emit(a, &adv, out);

    CHECK_INT(count(out, direct, sizeof direct), 2);   /* once alone, once after INDIR */
    CHECK_INT(count(out, indirect, sizeof indirect), 1);
    arena_free(a);
}

/* Identical entries still share one body. */
TEST(identical_entries_still_share)
{
    static const unsigned char indirect[] = { MES_OP | 0x80, 0, DONE_OP };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);

    add_entry(a, p, condact(a, MES_OP, "MES", 1, 0, 1, 0, 0));
    add_entry(a, p, condact(a, MES_OP, "MES", 1, 0, 1, 0, 0));
    emit(a, &adv, out);

    CHECK_INT(count(out, indirect, sizeof indirect), 1);
    arena_free(a);
}

int main(void)
{
    RUN(first_param_indirection_is_not_shared);
    RUN(second_param_indirection_is_not_shared);
    RUN(identical_entries_still_share);
    return test_summary("emit_proc");
}
