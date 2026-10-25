/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/srcmap.c - DSM v2 source map writer and CRC-32.
   Copyright (C) 2026 Dan Gibson. */
#include "back/srcmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int proc, file, line; } ProcRec;
typedef struct { unsigned off; int file, line; } EntryRec;
typedef struct { unsigned off, entry; int file, line, col; } CondRec;
typedef struct { char *name; long value; } SymRec;

#define DEFINE_LIST(T, name) \
    typedef struct { T *v; size_t n, cap; } name

DEFINE_LIST(char *, StrList);
DEFINE_LIST(ProcRec, ProcList);
DEFINE_LIST(EntryRec, EntryList);
DEFINE_LIST(CondRec, CondList);
DEFINE_LIST(SymRec, SymList);

struct SrcMap {
    StrList files;
    ProcList procs;
    EntryList entries;
    CondList conds;
    SymList syms;
};

uint32_t srcmap_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

/* Reserves one slot at the end of a list and returns its index; aborts on OOM. */
static size_t list_push(void **v, size_t *n, size_t *cap, size_t esz)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        void *p = realloc(*v, nc * esz);
        if (!p) {
            fputs("ndrc: out of memory\n", stderr);
            exit(1);
        }
        *v = p;
        *cap = nc;
    }
    return (*n)++;
}

#define PUSH(L) list_push((void **)&(L).v, &(L).n, &(L).cap, sizeof *(L).v)

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) {
        fputs("ndrc: out of memory\n", stderr);
        exit(1);
    }
    memcpy(p, s, n);
    return p;
}

SrcMap *srcmap_new(void)
{
    SrcMap *m = calloc(1, sizeof *m);
    if (!m) {
        fputs("ndrc: out of memory\n", stderr);
        exit(1);
    }
    return m;
}

void srcmap_free(SrcMap *m)
{
    size_t i;
    if (!m) return;
    for (i = 0; i < m->files.n; i++) free(m->files.v[i]);
    for (i = 0; i < m->syms.n; i++) free(m->syms.v[i].name);
    free(m->files.v);
    free(m->procs.v);
    free(m->entries.v);
    free(m->conds.v);
    free(m->syms.v);
    free(m);
}

int srcmap_file(SrcMap *m, const char *path)
{
    size_t i;
    for (i = 0; i < m->files.n; i++)
        if (strcmp(m->files.v[i], path) == 0) return (int)i;
    i = PUSH(m->files);
    m->files.v[i] = dup_str(path);
    return (int)i;
}

void srcmap_proc(SrcMap *m, int proc, int file, int line)
{
    size_t i = PUSH(m->procs);
    ProcRec *r = &m->procs.v[i];
    r->proc = proc; r->file = file; r->line = line;
}

void srcmap_entry(SrcMap *m, unsigned record_off, int file, int line)
{
    size_t i = PUSH(m->entries);
    EntryRec *r = &m->entries.v[i];
    r->off = record_off; r->file = file; r->line = line;
}

void srcmap_cond(SrcMap *m, unsigned off, unsigned entry_record_off,
                 int file, int line, int col)
{
    size_t i = PUSH(m->conds);
    CondRec *r = &m->conds.v[i];
    r->off = off; r->entry = entry_record_off;
    r->file = file; r->line = line; r->col = col;
}

void srcmap_sym(SrcMap *m, const char *name, long value)
{
    size_t i = PUSH(m->syms);
    m->syms.v[i].name = dup_str(name);
    m->syms.v[i].value = value;
}

int srcmap_write(const SrcMap *m, const char *path, const uint8_t *ddb, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) return 1;
    fprintf(f, "NDRC-DSM\t2\n");
    fprintf(f, "ddb\t%08lX\t%lX\n", (unsigned long)srcmap_crc32(ddb, len),
            (unsigned long)len);
    for (i = 0; i < m->files.n; i++)
        fprintf(f, "file\t%d\t%s\n", (int)i, m->files.v[i]);
    for (i = 0; i < m->procs.n; i++)
        fprintf(f, "proc\t%d\t%d\t%d\n", m->procs.v[i].proc,
                m->procs.v[i].file, m->procs.v[i].line);
    for (i = 0; i < m->entries.n; i++)
        fprintf(f, "entry\t%04X\t%d\t%d\n", m->entries.v[i].off,
                m->entries.v[i].file, m->entries.v[i].line);
    for (i = 0; i < m->conds.n; i++)
        fprintf(f, "cond\t%04X\t%04X\t%d\t%d\t%d\n", m->conds.v[i].off,
                m->conds.v[i].entry, m->conds.v[i].file,
                m->conds.v[i].line, m->conds.v[i].col);
    for (i = 0; i < m->syms.n; i++)
        fprintf(f, "sym\t%s\t%ld\n", m->syms.v[i].name, m->syms.v[i].value);
    if (ferror(f)) {
        fclose(f);
        return 1;
    }
    return fclose(f) != 0;
}
