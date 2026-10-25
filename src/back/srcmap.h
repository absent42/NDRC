/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/srcmap.h - DSM v2 source map writer and CRC-32.
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_BACK_SRCMAP_H
#define NDRC_BACK_SRCMAP_H

#include <stddef.h>
#include <stdint.h>

/* IEEE 802.3 CRC-32 (reflected, init and xorout 0xFFFFFFFF). */
uint32_t srcmap_crc32(const uint8_t *p, size_t n);

typedef struct SrcMap SrcMap;

SrcMap *srcmap_new(void);
void srcmap_free(SrcMap *m);

/* Returns the file index; the same path returns the same index. */
int srcmap_file(SrcMap *m, const char *path);
void srcmap_proc(SrcMap *m, int proc, int file, int line);
void srcmap_entry(SrcMap *m, unsigned record_off, int file, int line);
void srcmap_cond(SrcMap *m, unsigned off, unsigned entry_record_off,
                 int file, int line, int col);
void srcmap_sym(SrcMap *m, const char *name, long value);

/* `file` relative to the directory holding `dsm_path`, '/' separated;
   absolute when no relative path exists (another drive). Relative
   inputs resolve against the cwd. Returns malloc'd storage. */
char *srcmap_rel_path(const char *dsm_path, const char *file);

/* Writes the DSM v2 text for the given DDB image. 0 ok, non-zero on I/O failure. */
int srcmap_write(const SrcMap *m, const char *path, const uint8_t *ddb, size_t len);

#endif /* NDRC_BACK_SRCMAP_H */
