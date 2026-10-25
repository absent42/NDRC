/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/srcmap.c - DSM v2 source map writer and CRC-32.
   Copyright (C) 2026 Dan Gibson. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   /* getcwd */
#endif
#include "back/srcmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define NDRC_GETCWD _getcwd
#else
#include <unistd.h>
#define NDRC_GETCWD getcwd
#endif

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
        remove(path);
        return 1;
    }
    if (fclose(f) != 0) {
        remove(path);
        return 1;
    }
    return 0;
}

/* ---- relative paths ---- */

/* A path split into a root ("" relative, "/" rooted, "C:" a drive,
   "//host/share" UNC) and its segments with "." and ".." folded. */
typedef struct {
    char root[260];
    char *seg[256];
    int n;
    char *buf;
} SplitPath;

static int is_sep(char c) { return c == '/' || c == '\\'; }

static void split_path(SplitPath *p, const char *path)
{
    size_t len = strlen(path), i = 0, r = 0;
    char *s;

    p->buf = dup_str(path);
    s = p->buf;
    p->n = 0;
    p->root[0] = '\0';
    if (len >= 2 && s[1] == ':' && ((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z')) {
        p->root[r++] = s[0]; p->root[r++] = ':';
        i = 2;
    } else if (len >= 2 && is_sep(s[0]) && is_sep(s[1])) {
        int parts = 0;
        p->root[r++] = '/'; p->root[r++] = '/';
        i = 2;
        while (i < len && parts < 2 && r < sizeof p->root - 2) {
            if (is_sep(s[i])) { if (++parts == 2) break; p->root[r++] = '/'; }
            else p->root[r++] = s[i];
            i++;
        }
    }
    if (i < len && is_sep(s[i])) p->root[r++] = '/';
    p->root[r] = '\0';
    while (i < len) {
        size_t start;
        while (i < len && is_sep(s[i])) i++;
        start = i;
        while (i < len && !is_sep(s[i])) i++;
        if (i < len) s[i++] = '\0';
        if (start == i || s[start] == '\0' || strcmp(s + start, ".") == 0) continue;
        if (strcmp(s + start, "..") == 0 && p->n > 0 && strcmp(p->seg[p->n - 1], "..") != 0) {
            p->n--;
            continue;
        }
        if (p->n < 256) p->seg[p->n++] = s + start;
    }
}

static int name_eq(const char *a, const char *b)
{
#ifdef _WIN32
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

/* `path` made absolute against the cwd; malloc'd. */
static char *absolute(const char *path)
{
    char cwd[4096];
    char *out;
    size_t n;
    if (path[0] && path[1] == ':') return dup_str(path);
    if (NDRC_GETCWD(cwd, sizeof cwd) == NULL) return dup_str(path);
    if (is_sep(path[0])) {
#ifdef _WIN32
        /* rooted on the cwd's drive */
        if (cwd[0] && cwd[1] == ':') cwd[2] = '\0';
        else return dup_str(path);
#else
        return dup_str(path);
#endif
    }
    n = strlen(cwd) + 1 + strlen(path) + 1;
    out = malloc(n);
    if (!out) {
        fputs("ndrc: out of memory\n", stderr);
        exit(1);
    }
    snprintf(out, n, "%s/%s", cwd, path);
    return out;
}

static void append(char **out, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t nc = (*len + n + 1) * 2;
        char *p = realloc(*out, nc);
        if (!p) {
            fputs("ndrc: out of memory\n", stderr);
            exit(1);
        }
        *out = p;
        *cap = nc;
    }
    memcpy(*out + *len, s, n + 1);
    *len += n;
}

char *srcmap_rel_path(const char *dsm_path, const char *file)
{
    char *da = absolute(dsm_path), *fa = absolute(file);
    SplitPath d, f;
    char *out = NULL;
    size_t len = 0, cap = 0;
    int i, common = 0;

    split_path(&d, da);
    split_path(&f, fa);
    free(da);
    free(fa);
    if (d.n > 0) d.n--;   /* the DSM's own name */
    if (!name_eq(d.root, f.root)) {
        append(&out, &len, &cap, f.root);
        for (i = 0; i < f.n; i++) {
            if (i > 0) append(&out, &len, &cap, "/");
            append(&out, &len, &cap, f.seg[i]);
        }
    } else {
        while (common < d.n && common < f.n && name_eq(d.seg[common], f.seg[common])) common++;
        append(&out, &len, &cap, "");
        for (i = common; i < d.n; i++) append(&out, &len, &cap, "../");
        for (i = common; i < f.n; i++) {
            if (i > common) append(&out, &len, &cap, "/");
            append(&out, &len, &cap, f.seg[i]);
        }
    }
    free(d.buf);
    free(f.buf);
    return out;
}
