/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/jsonexport.h - Copyright (C) 2026 Dan Gibson.

   PORT: UJSONExport.pas (D:/DRC/src, branch nextdaad) - GenerateJSON,
   the front end's ONLY output stage.

   Every byte of this module's output is the byte-gate contract
   against reference drf.exe - whitespace, commas and escaping ported
   verbatim, defects included. */
#ifndef NDRC_FRONT_JSONEXPORT_H
#define NDRC_FRONT_JSONEXPORT_H

#include "diag.h"
#include "sintactic.h"

/* PORT: GenerateJSON(OutputFileName) (UJSONExport.pas:268-495). Opens
   `path` in BINARY mode and writes the whole file in one shot - CRLF
   UNCONDITIONALLY on every WriteLn-equivalent line, regardless of host
   platform (binary mode is what makes this controllable in C, see
   jsonexport.c). Remaining state comes from sintactic.h's getters,
   valid after sintactic_parse (and sintactic_fix_forward_labels, where
   forward SKIP labels exist).

   Returns 0 on success. A write failure reports via diag_fatal and
   returns exit class 2 - GenerateJSON itself has no such guard
   ({$I+} at the unit's top makes Rewrite/WriteLn raise on failure,
   uncaught); this is the FIDELITY POLICY's resource-safety carve-out. */
int jsonexport_write(Diag *d, const char *path, const FrontOptions *opts);

/* Render the document into the CALLER'S arena `a`: returns 0 on
   success and sets *out_data and *out_len to bytes owned by `a`,
   identical to what jsonexport_write would fwrite. The internal
   working arena is freed before returning - the returned buffer
   lives in `a` and nothing else survives. jsonexport_write is a thin
   wrapper over this: render into a scratch arena, one fwrite, free. */
int jsonexport_render(Arena *a, Diag *d, const FrontOptions *opts,
                       const unsigned char **out_data, size_t *out_len);

#endif /* NDRC_FRONT_JSONEXPORT_H */
