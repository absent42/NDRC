/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_nxtext.c - end-to-end tests for -nxtext (NextDAAD text
   format 2). Runs the built ndrc binary from the repo root, each run in
   a scratch directory (0.XMB lands in the cwd).
   Copyright (C) 2026 Dan Gibson. */
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "huff.h"

#ifdef _WIN32
#include <direct.h>
#define NDRC_BIN "\\ndrc.exe"
#define getcwd _getcwd
#define chdir _chdir
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define NDRC_BIN "/ndrc"
#define make_dir(p) mkdir(p, 0777)
#endif

#define BIGDDB   "tests/fixtures/BIGDDB.DSF"
#define OBJSHARE "tests/fixtures/OBJSHARE.DSF"
#define BLANK_EN "tests/fixtures/BLANK_EN.DSF"
#define NXMSG    "tests/fixtures/NXMSG.DSF"
#define BLANK_JSON "tests/fixtures/BLANK_EN.NEXTDAAD_EN_v3.json"

static char root[1024];

static void scratch_dir(char *buf, size_t bufsz, const char *name)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL) dir = getenv("TMP");
    if (dir == NULL) dir = ".";
    snprintf(buf, bufsz, "%s/%s", dir, name);
    make_dir(buf);
}

/* Runs ndrc with cwd = dir; paths in args must be absolute. */
static int run_in(const char *dir, const char *args, const char *logpath)
{
    char cmd[4096];
    int rc;

#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "\"\"%s" NDRC_BIN "\" %s > \"%s\" 2>&1\"",
             root, args, logpath);
#else
    snprintf(cmd, sizeof cmd, "\"%s" NDRC_BIN "\" %s > \"%s\" 2>&1",
             root, args, logpath);
#endif
    if (chdir(dir) != 0) return -1;
    rc = system(cmd);
    if (chdir(root) != 0) return -1;
#ifndef _WIN32
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static int files_equal(const char *pa, const char *pb)
{
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    int ca, cb, eq = 1;

    if (fa == NULL || fb == NULL) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    do {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb) { eq = 0; break; }
    } while (ca != EOF);
    fclose(fa);
    fclose(fb);
    return eq;
}

static int log_contains(const char *logpath, const char *needle)
{
    FILE *f = fopen(logpath, "rb");
    char line[1024];
    int found = 0;

    if (f == NULL) return 0;
    while (fgets(line, sizeof line, f) != NULL)
        if (strstr(line, needle) != NULL) { found = 1; break; }
    fclose(f);
    return found;
}

/* Whole file into buf; NULL on failure or overflow. */
static unsigned char *read_file(const char *path, unsigned char *buf,
                                size_t bufsz, long *len)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) return NULL;
    n = fread(buf, 1, bufsz, f);
    fclose(f);
    *len = (long)n;
    return n > 0 && n < bufsz ? buf : NULL;
}

static unsigned word_at(const unsigned char *p, long at)
{
    return (unsigned)p[at] | ((unsigned)p[at + 1] << 8);
}

/* Table kind: 0 sysmess, 1 messages, 2 locations, 3 objects. */
static int ddb_count(const unsigned char *ddb, int kind)
{
    return ddb[6 - kind];
}

static unsigned ddb_lookup_word(const unsigned char *ddb, int kind, int index)
{
    return word_at(ddb, word_at(ddb, 0x12 - 2 * kind) + 2L * index);
}

/* Format 1 text at src[p]: XOR 0xFF, 0x0A ends, tokens from the DDB. */
static int text1_at(const unsigned char *src, long slen, long p,
                    const unsigned char *ddb, long len, char *out,
                    size_t outsz)
{
    size_t o = 0;

    for (;;) {
        unsigned b;
        long t;
        int entry;
        if (p < 0 || p >= slen) return 0;
        b = src[p++] ^ 0xFFu;
        if (b == 0x0A) break;
        if (b < 128) {
            if (o + 1 >= outsz) return 0;
            out[o++] = (char)b;
            continue;
        }
        t = (long)word_at(ddb, 0x08);
        for (entry = 0; entry < (int)b - 127; entry++) {
            while (t < len && !(ddb[t] & 0x80)) t++;
            t++;
        }
        do {
            if (t >= len || o + 1 >= outsz) return 0;
            out[o++] = (char)(ddb[t] & 0x7F);
        } while (!(ddb[t++] & 0x80));
    }
    out[o] = '\0';
    return 1;
}

/* The format 2 text block, parsed from a DDB: part A is word 32 long,
   the region follows it and holds the block at word 8 (always 0). */
typedef struct {
    long at;                     /* block file offset = region start */
    unsigned flags, ntok, fingerprint, nsym;
    unsigned char lengths[HUFF_MAXSYM];
    const unsigned char *ddb;
    long len;
} Block2;

static int block2_parse(const unsigned char *ddb, long len, Block2 *b)
{
    long lb;
    unsigned i;

    memset(b, 0, sizeof *b);
    b->ddb = ddb;
    b->len = len;
    if (len < 0x22 || word_at(ddb, 0x08) != 0) return 0;
    b->at = (long)word_at(ddb, 0x20) + (long)word_at(ddb, 0x08);
    if (b->at + 7 > len || ddb[b->at] != 'H' || ddb[b->at + 1] != '2')
        return 0;
    b->flags = ddb[b->at + 2];
    b->ntok = word_at(ddb, b->at + 3);
    b->fingerprint = word_at(ddb, b->at + 5);
    b->nsym = (b->flags & 1) ? 511 : 256;
    lb = (b->flags & 1) ? 256 : 128;
    if (b->at + 7 + lb + 2L * (long)b->ntok > len) return 0;
    for (i = 0; i < b->nsym; i++) {
        unsigned v = ddb[b->at + 7 + i / 2];
        b->lengths[i] = (unsigned char)(i % 2 ? v >> 4 : v & 15);
    }
    return 1;
}

static long block2_entry(const Block2 *b, unsigned t)
{
    long lb = (b->flags & 1) ? 256 : 128;
    return b->at + (long)word_at(b->ddb, b->at + 7 + lb + 2L * t);
}

static int expand2(const Block2 *b, unsigned t, int depth, char *out,
                   size_t *o, size_t outsz)
{
    long e = block2_entry(b, t);
    unsigned n, i;

    if (e >= b->len) return 0;
    n = b->ddb[e];
    for (i = 1; i <= n; i++) {
        unsigned c = b->ddb[e + i];
        if (c >= 128) {
            /* One level; targets sit in 0-127 (text2_nest). */
            if (depth > 0 || c - 128 >= 128 || c - 128 >= b->ntok) return 0;
            if (!expand2(b, c - 128, 1, out, o, outsz)) return 0;
        } else {
            if (*o + 1 >= outsz) return 0;
            out[(*o)++] = (char)c;
        }
    }
    return 1;
}

/* Format 2 stream at src[p] (a region offset for DDB texts, src the
   region); *used = bytes it occupies. */
static int text2_at(const Block2 *b, const unsigned char *src, long slen,
                    long p, char *out, size_t outsz, long *used)
{
    size_t pos = 0, o = 0;

    if (p < 0 || p >= slen) return 0;
    for (;;) {
        int sym = huff_decode_one(b->lengths, b->nsym, src + p,
                                  (size_t)(slen - p), &pos);
        if (sym < 0) return 0;
        if (sym == 10) break;
        if (sym < 128) {
            if (o + 1 >= outsz) return 0;
            out[o++] = (char)sym;
        } else if ((unsigned)sym - 128 >= b->ntok ||
                   !expand2(b, (unsigned)sym - 128, 0, out, &o, outsz)) {
            return 0;
        }
    }
    out[o] = '\0';
    if (used) *used = (long)((pos + 7) / 8);
    return 1;
}

/* Region text through its lookup word. */
static int region_text(const Block2 *b, unsigned word, char *out,
                       size_t outsz)
{
    return text2_at(b, b->ddb + b->at, b->len - b->at, (long)word, out,
                    outsz, NULL);
}

static unsigned char ddb_a[65536], ddb_b[65536], xmb_a[65536], xmb_b[65536];

TEST(nxtext_header_and_block)
{
    char dir[512], dsf[1200], ctl[700], nx[700], nx3[700], logp[700];
    char args[3000], t1[4096], t2[4096];
    long n1 = 0, n2 = 0, n3 = 0;
    Block2 b;
    int kind, i;
    unsigned t;

    scratch_dir(dir, sizeof dir, "ndrc_nx_hdr");
    snprintf(dsf, sizeof dsf, "%s/%s", root, BIGDDB);
    snprintf(ctl, sizeof ctl, "%s/ctl.ddb", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(nx3, sizeof nx3, "%s/nx3.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);

    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens", dsf, ctl);
    CHECK_INT(run_in(dir, args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(log_contains(logp, "Text format 2: "));
    CHECK(!log_contains(logp, "not smaller"));
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v3", dsf, nx3);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(read_file(ctl, ddb_a, sizeof ddb_a, &n1) != NULL);
    CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n2) != NULL);
    CHECK(read_file(nx3, xmb_a, sizeof xmb_a, &n3) != NULL);
    if (n1 == 0 || n2 == 0 || n3 == 0) return;

    CHECK_INT(ddb_b[0], 0x82);
    CHECK_INT(xmb_a[0], 0x83);
    CHECK_INT(word_at(ddb_b, 0x08), 0);
    CHECK(word_at(ddb_b, 0x20) < (unsigned)n2);
    CHECK(block2_parse(ddb_b, n2, &b));
    if (b.at == 0) return;
    CHECK(b.ntok >= 1 && b.ntok <= 383);
    CHECK_INT(b.flags & 0xFE, 0);
    CHECK_INT(b.fingerprint,
              crc16_ccitt(ddb_b + b.at + 7, (b.flags & 1) ? 256 : 128));
    for (t = 1; t < b.ntok; t++)
        CHECK(block2_entry(&b, t) > block2_entry(&b, t - 1));

    /* Every DDB text decodes to the format 1 build's text. */
    for (kind = 0; kind < 4; kind++) {
        int cnt = kind == 3 ? (int)ddb_a[3] : ddb_count(ddb_a, kind);
        CHECK_INT(kind == 3 ? ddb_b[3] : ddb_count(ddb_b, kind), cnt);
        for (i = 0; i < cnt; i++) {
            t1[0] = t2[0] = '\0';
            CHECK(text1_at(ddb_a, n1, ddb_lookup_word(ddb_a, kind, i),
                           ddb_a, n1, t1, sizeof t1));
            CHECK(region_text(&b, ddb_lookup_word(ddb_b, kind, i),
                              t2, sizeof t2));
            if (strcmp(t1, t2) != 0) {
                CHECK_STR(t2, t1);
                return;
            }
        }
    }
}

TEST(nxtext_smaller_or_fallback)
{
    static const char *small[2] = { OBJSHARE, BLANK_EN };
    char dir[512], dsf[1200], ctl[700], nx[700], logp[700], args[3000];
    long s1, s2;
    int k;

    scratch_dir(dir, sizeof dir, "ndrc_nx_size");
    snprintf(ctl, sizeof ctl, "%s/ctl.ddb", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);

    snprintf(dsf, sizeof dsf, "%s/%s", root, BIGDDB);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens", dsf, ctl);
    CHECK_INT(run_in(dir, args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(!log_contains(logp, "format 1 emitted"));
    s1 = file_size(ctl);
    s2 = file_size(nx);
    CHECK(s1 > 0 && s2 > 0);
    CHECK(s2 * 100 <= s1 * 95);

    /* Small games: format 2 all the same (the index is the speed gain);
       a larger total is reported under -v only. */
    for (k = 0; k < 2; k++) {
        long n = 0;
        snprintf(dsf, sizeof dsf, "%s/%s", root, small[k]);
        snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens", dsf, ctl);
        CHECK_INT(run_in(dir, args, logp), 0);
        snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext", dsf, nx);
        CHECK_INT(run_in(dir, args, logp), 0);
        CHECK(!log_contains(logp, "format 1 emitted"));
        CHECK(!log_contains(logp, "bytes larger"));
        CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n) != NULL);
        if (n > 0) CHECK_INT(ddb_b[0], 0x82);
        CHECK(!files_equal(ctl, nx));
        snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v", dsf, nx);
        CHECK_INT(run_in(dir, args, logp), 0);
        CHECK(log_contains(logp, "Text format 2: "));
        if (file_size(nx) > file_size(ctl))
            CHECK(log_contains(logp, "bytes larger than format 1."));
    }
}

TEST(nxtext_blank_template)
{
    char dir[512], dsf[1200], nx[700], logp[700], args[3000];

    scratch_dir(dir, sizeof dir, "ndrc_nx_blank");
    snprintf(dsf, sizeof dsf, "%s/%s", root, BLANK_EN);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v3", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(file_size(nx) > 0);
}

TEST(nxtext_refusals)
{
    char dir[512], dsf[1200], json[1200], nx[700], logp[700], args[3000];
    long n = 0;

    scratch_dir(dir, sizeof dir, "ndrc_nx_ref");
    snprintf(dsf, sizeof dsf, "%s/%s", root, OBJSHARE);
    snprintf(json, sizeof json, "%s/%s", root, BLANK_JSON);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);

    remove(nx);
    snprintf(args, sizeof args, "ZX 48K EN \"%s\" \"%s\" -nxtext", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 2);
    CHECK(log_contains(logp, "-nxtext requires the NEXTDAAD target"));
    CHECK(file_size(nx) < 0);
    remove(nx);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -X", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 2);
    CHECK(log_contains(logp, "-X is not available with -nxtext"));
    CHECK(file_size(nx) < 0);
    remove(nx);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" --tok -nxtext", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 2);
    CHECK(log_contains(logp, "--tok is not available with -nxtext"));
    CHECK(file_size(nx) < 0);

    /* Case-sensitive, like the other NDRC options. */
    remove(nx);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -NXTEXT", dsf, nx);
    CHECK(run_in(dir, args, logp) != 0);
    CHECK(file_size(nx) < 0);

    /* --from-json takes it and refuses the same way. */
    remove(nx);
    snprintf(args, sizeof args, "--from-json ZX 48K EN \"%s\" \"%s\" -nxtext", json, nx);
    CHECK_INT(run_in(dir, args, logp), 2);
    CHECK(log_contains(logp, "-nxtext requires the NEXTDAAD target"));
    CHECK(file_size(nx) < 0);
    remove(nx);
    snprintf(args, sizeof args, "--from-json NEXTDAAD EN \"%s\" \"%s\" -nxtext", json, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(!log_contains(logp, "format 1 emitted"));
    CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n) != NULL);
    if (n > 0) CHECK_INT(ddb_b[0], 0x83);
}

/* Format 1 XMB texts, in file order. */
static int xmb1_texts(const unsigned char *x, long n, const unsigned char *ddb,
                      long dlen, char texts[][1024], int max)
{
    long p = 0;
    int k = 0;

    while (p < n && k < max) {
        if (!text1_at(x, n, p, ddb, dlen, texts[k], sizeof texts[k])) return -1;
        while (p < n && (x[p] ^ 0xFFu) != 0x0A) p++;
        p++;
        k++;
    }
    return k;
}

/* True when "XMES offset" (opcode 120, LSB, MSB) occurs in the DDB. */
static int has_xmes_final(const unsigned char *ddb, long n, long off)
{
    long i;
    for (i = 0; i + 2 < n; i++)
        if (ddb[i] == 120 && ddb[i + 1] == (off & 0xFF) &&
            ddb[i + 2] == ((off >> 8) & 0xFF))
            return 1;
    return 0;
}

static char ctl_texts[16][1024];

TEST(nxtext_xmb_header)
{
    char dir[512], dsf[1200], ctl[700], nx[700], xmb[700], logp[700];
    char args[3000], t2[1024];
    long n1 = 0, n2 = 0, x1 = 0, x2 = 0, p = 4;
    int k, count;
    Block2 b;

    scratch_dir(dir, sizeof dir, "ndrc_nx_xmb");
    snprintf(dsf, sizeof dsf, "%s/%s", root, NXMSG);
    snprintf(ctl, sizeof ctl, "%s/ctl.ddb", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(xmb, sizeof xmb, "%s/0.XMB", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);

    remove(xmb);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens -v3", dsf, ctl);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(read_file(ctl, ddb_a, sizeof ddb_a, &n1) != NULL);
    CHECK(read_file(xmb, xmb_a, sizeof xmb_a, &x1) != NULL);
    remove(xmb);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v3", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n2) != NULL);
    CHECK(read_file(xmb, xmb_b, sizeof xmb_b, &x2) != NULL);
    remove(xmb);
    if (n1 == 0 || n2 == 0 || x1 == 0 || x2 < 4) return;

    count = xmb1_texts(xmb_a, x1, ddb_a, n1, ctl_texts, 16);
    CHECK_INT(count, 6);
    CHECK_INT(ddb_b[0], 0x83);
    CHECK(block2_parse(ddb_b, n2, &b));
    CHECK(xmb_b[0] == 'X' && xmb_b[1] == '2');
    CHECK_INT(word_at(xmb_b, 2), b.fingerprint);
    CHECK_INT(word_at(xmb_b, 2),
              crc16_ccitt(ddb_b + b.at + 7, (b.flags & 1) ? 256 : 128));
    CHECK(x2 < x1);
    for (k = 0; k < count; k++) {
        long used = 0;
        CHECK(has_xmes_final(ddb_b, n2, p));
        CHECK(text2_at(&b, xmb_b, x2, p, t2, sizeof t2, &used));
        CHECK_STR(t2, ctl_texts[k]);
        p += used;
    }
    CHECK_INT(p, x2);
}

/* Replaces xmessage 0's JSON text with n pseudo-random letters. */
static int oversize_json(const char *in, const char *out, size_t n)
{
    static unsigned char buf[262144];
    const char *needle = "The old lighthouse keeper";
    char *s, *x, *e;
    long len = 0;
    FILE *f;
    unsigned seed = 12345u;
    size_t i;

    if (read_file(in, buf, sizeof buf - 1, &len) == NULL) return 0;
    buf[len] = '\0';
    x = strstr((char *)buf, "\"xmessages\"");
    s = x ? strstr(x, needle) : NULL;
    e = s ? strchr(s, '"') : NULL;
    if (e == NULL) return 0;
    f = fopen(out, "wb");
    if (f == NULL) return 0;
    fwrite(buf, 1, (size_t)(s - (char *)buf), f);
    for (i = 0; i < n; i++) {
        static const char set[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        seed = seed * 1103515245u + 12345u;
        fputc(set[(seed >> 16) % (sizeof set - 1)], f);
    }
    fputs(e, f);
    return fclose(f) == 0;
}

TEST(nxtext_xmb_oversize_refused)
{
    char dir[512], dsf[1200], js[700], big[700], nx[700], xmb[700];
    char logp[700], args[3000];

    scratch_dir(dir, sizeof dir, "ndrc_nx_big");
    snprintf(dsf, sizeof dsf, "%s/%s", root, NXMSG);
    snprintf(js, sizeof js, "%s/nx.json", dir);
    snprintf(big, sizeof big, "%s/big.json", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(xmb, sizeof xmb, "%s/0.XMB", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);

    snprintf(args, sizeof args, "--to-json NEXTDAAD \"%s\" \"%s\"", dsf, js);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(oversize_json(js, big, 20000));

    /* Valid input: format 1 takes it. */
    snprintf(args, sizeof args, "--from-json NEXTDAAD EN \"%s\" \"%s\" -auto-tokens",
             big, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    remove(xmb);
    remove(nx);
    snprintf(args, sizeof args, "--from-json NEXTDAAD EN \"%s\" \"%s\" -nxtext",
             big, nx);
    CHECK_INT(run_in(dir, args, logp), 2);
    CHECK(log_contains(logp, "XMESSAGE 0 exceeds 8K coded"));
    CHECK(file_size(xmb) < 0);
    CHECK(file_size(nx) < 0);
}

/* Builds dsf with opts in dir and compares the DDB (and 0.XMB when
   gxmb is set) with the goldens. */
static int same_as_golden(const char *dir, const char *dsf, const char *opts,
                          const char *gddb, const char *gxmb)
{
    char src[1200], out[700], xmb[700], logp[700], g[1200], args[3000];
    int ok;

    snprintf(src, sizeof src, "%s/%s", root, dsf);
    snprintf(out, sizeof out, "%s/f1.ddb", dir);
    snprintf(xmb, sizeof xmb, "%s/0.XMB", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);
    remove(xmb);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" %s", src, out, opts);
    if (run_in(dir, args, logp) != 0) return 0;
    snprintf(g, sizeof g, "%s/%s", root, gddb);
    ok = files_equal(out, g);
    if (gxmb != NULL) {
        snprintf(g, sizeof g, "%s/%s", root, gxmb);
        ok = ok && files_equal(xmb, g);
    } else {
        ok = ok && file_size(xmb) < 0;
    }
    remove(xmb);
    return ok;
}

/* Writes BLANK_EN to out with its empty /MTX replaced by nmsg copies of
   msgs[k % nset], and pro7 (may be NULL) inserted before /END. */
static int blank_variant(const char *out, const char *const *msgs,
                         int nset, int nmsg, const char *pro7)
{
    static unsigned char buf[262144];
    char path[1200];
    const char *s, *m, *z, *e;
    long len = 0;
    FILE *f;
    int k;

    snprintf(path, sizeof path, "%s/%s", root, BLANK_EN);
    if (read_file(path, buf, sizeof buf - 1, &len) == NULL) return 0;
    buf[len] = '\0';
    s = (const char *)buf;
    m = strstr(s, "/MTX");
    z = m ? strstr(m, "/0 \"\"") : NULL;
    e = strstr(s, "\n/END");
    if (z == NULL || e == NULL || e < z) return 0;
    f = fopen(out, "wb");
    if (f == NULL) return 0;
    fwrite(s, 1, (size_t)(z - s), f);
    for (k = 0; k < nmsg; k++)
        fprintf(f, "/%d \"%s\"\r\n", k, msgs[k % nset]);
    z = strchr(z, '\n') + 1;
    fwrite(z, 1, (size_t)(e + 1 - z), f);
    if (pro7 != NULL) fputs(pro7, f);
    fputs(e + 1, f);
    return fclose(f) == 0;
}

/* The DUP fixture: 6 texts x 31 copies. */
static const char *dup_msgs[6] = {
    "You are in a small room.", "You are in a long corridor.",
    "You are in a dark cellar.", "You are in a bright hall.",
    "You are in a narrow passage.", "You are in a cold kitchen."
};

TEST(nxtext_duplicates_reported)
{
    /* 6 texts x 31 copies: shared duplicates count once in the
       reported difference, which is the file difference exactly. */
    char dir[512], dsf[700], ctl[700], nx[700], logp[700], args[3000];
    long s1, s2, n = 0;

    scratch_dir(dir, sizeof dir, "ndrc_nx_dup");
    snprintf(dsf, sizeof dsf, "%s/DUP.DSF", dir);
    snprintf(ctl, sizeof ctl, "%s/ctl.ddb", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);
    CHECK(blank_variant(dsf, dup_msgs, 6, 186, NULL));

    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens", dsf, ctl);
    CHECK_INT(run_in(dir, args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    s1 = file_size(ctl);
    s2 = file_size(nx);
    CHECK(s1 > 0 && s2 > 0);
    CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n) != NULL);
    if (n > 0) CHECK_INT(ddb_b[0], 0x82);
    if (s2 > s1) {
        snprintf(args, sizeof args,
                 "Text format 2 is %ld bytes larger than format 1.", s2 - s1);
        CHECK(log_contains(logp, args));
    } else {
        CHECK(!log_contains(logp, "bytes larger"));
    }
}

TEST(region_layout)
{
    char dir[512], dsf[1200], ctl[700], nx[700], logp[700];
    char args[3000], t1[4096], t2[4096];
    long n1 = 0, n2 = 0, part_a, region;
    Block2 b;
    int kind, i, w;

    scratch_dir(dir, sizeof dir, "ndrc_nx_region");
    snprintf(dsf, sizeof dsf, "%s/%s", root, OBJSHARE);
    snprintf(ctl, sizeof ctl, "%s/ctl.ddb", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);

    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens", dsf, ctl);
    CHECK_INT(run_in(dir, args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(log_contains(logp, "Text region       ["));
    CHECK(log_contains(logp, "Tokens            [0x0000]"));
    CHECK(read_file(ctl, ddb_a, sizeof ddb_a, &n1) != NULL);
    CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n2) != NULL);
    if (n1 == 0 || n2 < 0x22) return;

    part_a = (long)word_at(ddb_b, 0x20);
    region = n2 - part_a;
    CHECK_INT(word_at(ddb_b, 0x08), 0);
    CHECK(part_a >= 60 && region >= 7 + 128);
    if (part_a >= n2) return;
    CHECK_INT(part_a + region, n2);
    CHECK_INT(ddb_b[part_a], 'H');
    CHECK_INT(ddb_b[part_a + 1], '2');
    /* Part A ends with the processes: every header table sits before A. */
    for (w = 0x0A; w < 0x20; w += 2)
        CHECK(word_at(ddb_b, w) < (unsigned)part_a);
    snprintf(args, sizeof args, "Part A is %ld bytes, text region is %ld bytes.",
             part_a, region);
    CHECK(log_contains(logp, args));
    snprintf(args, sizeof args, "Total DDB size is %ld bytes.", n2);
    CHECK(log_contains(logp, args));
    CHECK(block2_parse(ddb_b, n2, &b));
    if (b.at != part_a) return;

    for (kind = 0; kind < 4; kind++) {
        int cnt = kind == 3 ? (int)ddb_a[3] : ddb_count(ddb_a, kind);
        CHECK_INT(kind == 3 ? ddb_b[3] : ddb_count(ddb_b, kind), cnt);
        for (i = 0; i < cnt; i++) {
            unsigned word = ddb_lookup_word(ddb_b, kind, i);
            t1[0] = t2[0] = '\0';
            CHECK(word > 0 && (long)word < region);
            CHECK(text1_at(ddb_a, n1, ddb_lookup_word(ddb_a, kind, i),
                           ddb_a, n1, t1, sizeof t1));
            CHECK(region_text(&b, word, t2, sizeof t2));
            if (strcmp(t1, t2) != 0) {
                CHECK_STR(t2, t1);
                return;
            }
        }
    }
}

TEST(region_shares_duplicates)
{
    char dir[512], dsf[700], nx[700], logp[700], args[3000], txt[256];
    long n = 0;
    Block2 b;
    int k;

    scratch_dir(dir, sizeof dir, "ndrc_nx_rdup");
    snprintf(dsf, sizeof dsf, "%s/DUP.DSF", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);
    CHECK(blank_variant(dsf, dup_msgs, 6, 186, NULL));
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(read_file(nx, ddb_b, sizeof ddb_b, &n) != NULL);
    if (n < 0x22) return;
    CHECK_INT(ddb_count(ddb_b, 1), 186);
    for (k = 0; k + 6 < 186; k++)
        CHECK_INT(ddb_lookup_word(ddb_b, 1, k), ddb_lookup_word(ddb_b, 1, k + 6));
    for (k = 0; k + 1 < 6; k++)
        CHECK(ddb_lookup_word(ddb_b, 1, k) != ddb_lookup_word(ddb_b, 1, k + 1));
    CHECK(block2_parse(ddb_b, n, &b));
    for (k = 0; k < 7; k += 6) {
        txt[0] = '\0';
        CHECK(region_text(&b, ddb_lookup_word(ddb_b, 1, k), txt, sizeof txt));
        CHECK_STR(txt, dup_msgs[0]);
    }
}

/* Seeded pseudo-random letters, digits, space and full stop; no real text. */
static void synth_text(char *out, int len, unsigned *seed)
{
    int i;

    for (i = 0; i < len; i++) {
        *seed = *seed * 1103515245u + 12345u;
        unsigned r = (*seed >> 16) % 64;
        out[i] = r < 26 ? (char)('a' + r) : r < 52 ? (char)('A' + r - 26)
               : r < 62 ? (char)('0' + r - 52) : r == 62 ? ' ' : '.';
    }
    out[len] = '\0';
}

/* PRO 7 text of n entries of 10 PLUS condacts each. */
static char *synth_pro7(int n)
{
    size_t sz = 32 + (size_t)n * 400;
    char *p = malloc(sz), *w = p;
    int i, j;

    if (p == NULL) return NULL;
    w += sprintf(w, "/PRO 7\r\n\r\n");
    /* Entries differ so the compiler cannot share them. */
    for (i = 0; i < n; i++) {
        w += sprintf(w, "> _       _     PLUS    %d %d\r\n", 50 + i % 200, 1);
        w += sprintf(w, "                PLUS    %d %d\r\n", 50 + i / 200, 1);
        for (j = 2; j < 10; j++)
            w += sprintf(w, "                PLUS    %d %d\r\n", 50 + (i * 7 + j * 31) % 200, j);
    }
    return p;
}

/* BLANK_EN variant with nmsg messages of mlen synthetic characters and
   nent process entries; runs -nxtext into dir/out.ddb. */
static char synth_store[250][600];

static int synth_run(const char *dir, int nmsg, int mlen, int nent, int xmes,
                     char *dsf, char *out, char *logp, int *rc)
{
    static const char xpro[] =
        "/PRO 7\r\n\r\n> _       _     XMES    \"Hello.\"\r\n";
    const char *ptrs[250];
    char args[3000], *pro7 = NULL;
    unsigned seed = 12345u;
    int k, ok;

    for (k = 0; k < nmsg; k++) {
        synth_text(synth_store[k], mlen, &seed);
        ptrs[k] = synth_store[k];
    }
    if (nent > 0 && (pro7 = synth_pro7(nent)) == NULL) return 0;
    if (xmes && nent == 0 && (pro7 = strdup(xpro)) == NULL) return 0;
    snprintf(dsf, 700, "%s/SYN.DSF", dir);
    snprintf(out, 700, "%s/out.ddb", dir);
    snprintf(logp, 700, "%s/log.txt", dir);
    remove(out);
    ok = blank_variant(dsf, ptrs, nmsg, nmsg, pro7);
    free(pro7);
    if (!ok) return 0;
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -v", dsf, out);
    *rc = run_in(dir, args, logp);
    return 1;
}

#define NENT_BIG 1800

TEST(file_over_64k_fits_both_budgets)
{
    char dir[512], dsf[700], out[700], logp[700];
    long n = 0, part_a, region;
    int rc = -1;

    scratch_dir(dir, sizeof dir, "ndrc_nx_big");
    CHECK(synth_run(dir, 200, 100, NENT_BIG, 0, dsf, out, logp, &rc));
    CHECK_INT(rc, 0);
    CHECK(log_contains(logp, "Text format 2:"));
    n = file_size(out);
    CHECK(n > 65535);
    {
        static unsigned char big[140000];
        long m = 0;
        if (read_file(out, big, sizeof big, &m) == NULL || m < 0x22) return;
        part_a = (long)word_at(big, 0x20);
        region = m - part_a;
        CHECK(part_a > 0 && region > 0 && region < 65536);
        CHECK_INT(word_at(big, 0x08), 0);
        {
            Block2 b;
            char t[1024];
            int ks[2] = { 0, 199 }, q;

            CHECK(block2_parse(big, m, &b));
            for (q = 0; q < 2; q++) {
                CHECK(region_text(&b, ddb_lookup_word(big, 1, ks[q]), t,
                                  sizeof t));
                CHECK_STR(t, synth_store[ks[q]]);
            }
        }
        {
            char want[128];
            snprintf(want, sizeof want,
                     "Part A is %ld bytes, text region is %ld bytes.",
                     part_a, region);
            CHECK(log_contains(logp, want));
        }
    }
}

TEST(region_over_limit_fails)
{
    char dir[512], dsf[700], out[700], logp[700];
    int rc = -1;

    scratch_dir(dir, sizeof dir, "ndrc_nx_rover");
    CHECK(synth_run(dir, 250, 400, 0, 0, dsf, out, logp, &rc));
    CHECK_INT(rc, 2);
    CHECK(log_contains(logp, "Text region goes "));
    CHECK(log_contains(logp, " bytes over the 65535 limit"));
    CHECK(file_size(out) < 0);
}

TEST(region_over_limit_with_xmessage_writes_nothing)
{
    char dir[512], dsf[700], out[700], logp[700], xmb[700];
    int rc = -1;

    scratch_dir(dir, sizeof dir, "ndrc_nx_rovx");
    snprintf(xmb, sizeof xmb, "%s/0.XMB", dir);
    remove(xmb);
    CHECK(synth_run(dir, 250, 400, 0, 1, dsf, out, logp, &rc));
    CHECK_INT(rc, 2);
    CHECK(log_contains(logp, "bytes over the 65535 limit"));
    CHECK(file_size(out) < 0);
    CHECK(file_size(xmb) < 0);
}

TEST(part_a_over_limit_fails)
{
    char dir[512], dsf[700], out[700], logp[700];
    int rc = -1;

    scratch_dir(dir, sizeof dir, "ndrc_nx_aover");
    CHECK(synth_run(dir, 2, 20, 2300, 0, dsf, out, logp, &rc));
    CHECK_INT(rc, 2);
    CHECK(log_contains(logp, "memory address boundary"));
    CHECK(file_size(out) < 0);
}

TEST(forced_base_word32_is_length)
{
    char dir[512], dsf[1200], nx[700], nb[700], logp[700], args[3000];
    long n1 = 0, n2 = 0;

    scratch_dir(dir, sizeof dir, "ndrc_nx_base");
    snprintf(dsf, sizeof dsf, "%s/%s", root, OBJSHARE);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(nb, sizeof nb, "%s/nb.ddb", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext -b=1000", dsf, nb);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(read_file(nx, ddb_a, sizeof ddb_a, &n1) != NULL);
    CHECK(read_file(nb, ddb_b, sizeof ddb_b, &n2) != NULL);
    if (n1 < 0x22 || n2 < 0x22) return;
    CHECK_INT(n2, n1);
    CHECK_INT(word_at(ddb_b, 0x20), word_at(ddb_a, 0x20));
    CHECK_INT(word_at(ddb_b, 0x08), 0);
    CHECK(word_at(ddb_b, 0x20) < (unsigned)n2);
    /* Table addresses do move with the base. */
    CHECK_INT(word_at(ddb_b, 0x0A), word_at(ddb_a, 0x0A) + 1000);
}

#define GNX "tests/goldens/NXTEXT/"

TEST(format1_untouched)
{
    static const char *none[1] = { "" };
    static const char pro7[] =
        "/PRO 7\r\n\r\n> _       _     XMES    \"Hello.\"\r\n";
    char dir[512], dsf[700], ctl[700], nx[700], xmb[700], xctl[700];
    char logp[700], args[3000];
    long n = 0;

    /* Goldens: NDRC 3ecdf80 builds of the same fixtures. */
    scratch_dir(dir, sizeof dir, "ndrc_nx_f1");
    CHECK(same_as_golden(dir, OBJSHARE, "", GNX "OBJSHARE_plain.ddb", NULL));
    CHECK(same_as_golden(dir, OBJSHARE, "-auto-tokens", GNX "OBJSHARE_auto.ddb", NULL));
    CHECK(same_as_golden(dir, BLANK_EN, "", "tests/goldens/BLANK_EN/NEXTDAAD_EN_v2_opt.ddb", NULL));
    CHECK(same_as_golden(dir, BLANK_EN, "-auto-tokens", GNX "BLANK_EN_auto.ddb", NULL));
    CHECK(same_as_golden(dir, BIGDDB, "", "tests/goldens/BIGDDB/NEXTDAAD_EN_v2_opt.ddb", NULL));
    CHECK(same_as_golden(dir, BIGDDB, "-auto-tokens", GNX "BIGDDB_auto.ddb", NULL));
    CHECK(same_as_golden(dir, NXMSG, "", GNX "NXMSG_plain.ddb", GNX "NXMSG_plain.0.XMB"));
    CHECK(same_as_golden(dir, NXMSG, "-auto-tokens", GNX "NXMSG_auto.ddb", GNX "NXMSG_auto.0.XMB"));

    /* A short XMESSAGE still codes: format 2 DDB and "X2" XMB. */
    snprintf(dsf, sizeof dsf, "%s/XSHORT.DSF", dir);
    snprintf(ctl, sizeof ctl, "%s/ctl.ddb", dir);
    snprintf(nx, sizeof nx, "%s/nx.ddb", dir);
    snprintf(xmb, sizeof xmb, "%s/0.XMB", dir);
    snprintf(xctl, sizeof xctl, "%s/ctl.XMB", dir);
    snprintf(logp, sizeof logp, "%s/log.txt", dir);
    CHECK(blank_variant(dsf, none, 1, 1, pro7));
    remove(xmb);
    remove(xctl);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -auto-tokens", dsf, ctl);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(rename(xmb, xctl) == 0);
    snprintf(args, sizeof args, "NEXTDAAD EN \"%s\" \"%s\" -nxtext", dsf, nx);
    CHECK_INT(run_in(dir, args, logp), 0);
    CHECK(!log_contains(logp, "format 1 emitted"));
    CHECK(file_size(xctl) > 0);
    CHECK(!files_equal(ctl, nx));
    CHECK(read_file(xmb, xmb_a, sizeof xmb_a, &n) != NULL);
    if (n >= 4) {
        CHECK_INT(xmb_a[0], 'X');
        CHECK_INT(xmb_a[1], '2');
    }
    remove(xmb);
    remove(xctl);
}

int main(void)
{
    if (getcwd(root, sizeof root) == NULL) return 1;
    RUN(nxtext_header_and_block);
    RUN(nxtext_smaller_or_fallback);
    RUN(nxtext_blank_template);
    RUN(nxtext_refusals);
    RUN(nxtext_xmb_header);
    RUN(nxtext_xmb_oversize_refused);
    RUN(nxtext_duplicates_reported);
    RUN(region_layout);
    RUN(region_shares_duplicates);
    RUN(file_over_64k_fits_both_budgets);
    RUN(region_over_limit_fails);
    RUN(region_over_limit_with_xmessage_writes_nothing);
    RUN(part_a_over_limit_fails);
    RUN(forced_base_word32_is_length);
    RUN(format1_untouched);
    return test_summary("nxtext");
}
