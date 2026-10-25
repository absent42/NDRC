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

static void emit_map(Arena *a, Adventure *adv, Str *out, ProcAddrs *map)
{
    Diag *d = diag_new(a);
    const Target *t = target_lookup("NEXTDAAD", NULL);
    long addr = 0;

    CHECK(t != NULL);
    if (t == NULL) return;
    emit_processes(out, &addr, d, t, adv, 0, map);
    CHECK_INT(diag_error_count(d), 0);
}

static void emit(Arena *a, Adventure *adv, Str *out)
{
    emit_map(a, adv, out, NULL);
}

static Condact *src_at(Condact *c, int src)
{
    c->src = src;
    return c;
}

/* A new empty entry of process p; condacts get src in push order. */
static ProcEntry *new_entry(Arena *a, Process *p)
{
    ProcEntry *e = arena_calloc(a, sizeof(*e));
    e->Entry = "_";
    e->Verb = 255;
    e->Noun = 255;
    e->condacts = vec_new_Condact(a);
    vec_push_ProcEntry(p->entries, e);
    return e;
}

static void push(ProcEntry *e, Condact *c)
{
    vec_push_Condact(e->condacts, src_at(c, (int)vec_len_Condact(e->condacts)));
}

static void check_cond(const ProcAddrs *m, size_t i, long addr, int src,
                       long entry, long record)
{
    const ProcCondAddr *c = vec_at_ProcCondAddr(m->conds, i);
    CHECK(c != NULL);
    if (c == NULL) return;
    CHECK_INT(c->addr, addr);
    CHECK_INT(c->src, src);
    CHECK_INT(c->owner->entry, entry);
    CHECK_INT(c->owner->record, record);
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

/* Entry 2 (MES 2 DONE) is entry 1's tail: it emits nothing and its
   conds land on entry 1's offsets 2 and 4 under its own record. */
TEST(srcmap_shared_tail_mid_body)
{
    static const unsigned char want[] = {
        MES_OP, 1, MES_OP, 2, DONE_OP,
        255, 255, 0, 0, 255, 255, 2, 0, 0,
        5, 0
    };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);
    ProcAddrs m = { NULL, NULL };
    ProcEntry *e1 = new_entry(a, p);
    ProcEntry *e2 = new_entry(a, p);

    push(e1, condact(a, MES_OP, "MES", 1, 1, 0, 0, 0));
    push(e1, condact(a, MES_OP, "MES", 1, 2, 0, 0, 0));
    push(e1, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    push(e2, condact(a, MES_OP, "MES", 1, 2, 0, 0, 0));
    push(e2, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    emit_map(a, &adv, out, &m);

    CHECK_INT(str_len(out), sizeof want);
    if (str_len(out) == sizeof want) CHECK_MEM(str_bytes(out), want, sizeof want);
    CHECK(m.entries != NULL && m.conds != NULL);
    if (m.entries == NULL || m.conds == NULL) { arena_free(a); return; }
    CHECK_INT(vec_len_ProcEntryAddr(m.entries), 2);
    CHECK_INT(vec_at_ProcEntryAddr(m.entries, 0)->record, 5);
    CHECK_INT(vec_at_ProcEntryAddr(m.entries, 1)->record, 9);
    CHECK_INT(vec_len_ProcCondAddr(m.conds), 5);
    check_cond(&m, 0, 0, 0, 0, 5);
    check_cond(&m, 1, 2, 1, 0, 5);
    check_cond(&m, 2, 4, 2, 0, 5);
    check_cond(&m, 3, 2, 0, 1, 9);
    check_cond(&m, 4, 4, 1, 1, 9);
    arena_free(a);
}

/* XDATA "100,1,2,3" becomes three LETs, all carrying the XDATA's src. */
TEST(srcmap_xdata_expansion_keeps_line)
{
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);
    ProcAddrs m = { NULL, NULL };
    ProcEntry *e = new_entry(a, p);
    Message *s = arena_calloc(a, sizeof(*s));

    s->Text = str_new(a);
    str_append(s->Text, "100,1,2,3");
    vec_push_Message(adv.other_strings, s);
    push(e, condact(a, MES_OP, "MES", 1, 0, 0, 0, 0));
    push(e, condact(a, 142, "XDATA", 1, 0, 0, 0, 0));
    push(e, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    emit_map(a, &adv, out, &m);

    CHECK(m.conds != NULL);
    if (m.conds == NULL) { arena_free(a); return; }
    CHECK_INT(vec_len_ProcCondAddr(m.conds), 5);
    check_cond(&m, 0, 0, 0, 0, 12);
    check_cond(&m, 1, 2, 1, 0, 12);
    check_cond(&m, 2, 5, 1, 0, 12);
    check_cond(&m, 3, 8, 1, 0, 12);
    check_cond(&m, 4, 11, 2, 0, 12);
    arena_free(a);
}

/* USERPTR emits nothing, nor does a condact after a terminator; an
   INDIR-prefixed condact is recorded at the prefix and opcode bytes. */
TEST(srcmap_unemitted_have_no_record)
{
    static const unsigned char want[] = {
        INDIR_OP, 200, LET_OP, 100, 200, DONE_OP
    };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);
    ProcAddrs m = { NULL, NULL };
    ProcEntry *e = new_entry(a, p);

    push(e, condact(a, 256, "#userptr", 1, 3, 0, 0, 0));
    push(e, condact(a, LET_OP, "LET", 2, 100, 0, 200, 1));
    push(e, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    push(e, condact(a, MES_OP, "MES", 1, 0, 0, 0, 0));
    emit_map(a, &adv, out, &m);

    CHECK(str_len(out) >= sizeof want);
    if (str_len(out) >= sizeof want) CHECK_MEM(str_bytes(out), want, sizeof want);
    CHECK(m.conds != NULL);
    if (m.conds == NULL) { arena_free(a); return; }
    CHECK_INT(vec_len_ProcCondAddr(m.conds), 3);
    check_cond(&m, 0, 0, 1, 0, 6);
    check_cond(&m, 1, 2, 1, 0, 6);
    check_cond(&m, 2, 5, 2, 0, 6);
    arena_free(a);
}

/* LET 100 @101: one record at the INDIR byte and one at the LET opcode,
   in the emitting entry and in an entry sharing its bytes. */
TEST(srcmap_indir_records_prefix_and_opcode)
{
    static const unsigned char want[] = { INDIR_OP, 101, LET_OP, 100, 101, DONE_OP };
    Arena *a = arena_new(0);
    Str *out = str_new(a);
    Adventure adv;
    Process *p = new_adventure(a, &adv);
    ProcAddrs m = { NULL, NULL };
    ProcEntry *e1 = new_entry(a, p);
    ProcEntry *e2 = new_entry(a, p);

    push(e1, condact(a, LET_OP, "LET", 2, 100, 0, 101, 1));
    push(e1, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    push(e2, condact(a, LET_OP, "LET", 2, 100, 0, 101, 1));
    push(e2, condact(a, DONE_OP, "DONE", 0, 0, 0, 0, 0));
    emit_map(a, &adv, out, &m);

    CHECK(str_len(out) >= sizeof want);
    if (str_len(out) >= sizeof want) CHECK_MEM(str_bytes(out), want, sizeof want);
    CHECK(m.conds != NULL);
    if (m.conds == NULL) { arena_free(a); return; }
    CHECK_INT(vec_len_ProcCondAddr(m.conds), 6);
    check_cond(&m, 0, 0, 0, 0, 6);
    check_cond(&m, 1, 2, 0, 0, 6);
    check_cond(&m, 2, 5, 1, 0, 6);
    check_cond(&m, 3, 0, 0, 1, 10);
    check_cond(&m, 4, 2, 0, 1, 10);
    check_cond(&m, 5, 5, 1, 1, 10);
    arena_free(a);
}

int main(void)
{
    RUN(first_param_indirection_is_not_shared);
    RUN(second_param_indirection_is_not_shared);
    RUN(identical_entries_still_share);
    RUN(srcmap_shared_tail_mid_body);
    RUN(srcmap_xdata_expansion_keeps_line);
    RUN(srcmap_unemitted_have_no_record);
    RUN(srcmap_indir_records_prefix_and_opcode);
    return test_summary("emit_proc");
}
