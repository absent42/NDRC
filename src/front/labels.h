/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/labels.h - Copyright (C) 2026 Dan Gibson.

   PORT: ULabelList.pas (D:/DRC/src, branch nextdaad) - the label table
   ($name tokens, SKIP/JUMP targets).
   Unlike symbols.c, keys here are CASE-SENSITIVE and stored WITH their
   '$' prefix (the lexer's T_LABEL token text is taken verbatim, so
   `$Foo` and `$FOO` are different labels) - do not fold or strip
   anything before calling in. */
#ifndef NDRC_FRONT_LABELS_H
#define NDRC_FRONT_LABELS_H

#include "arena.h"
#include "diag.h"

/* PORT: TLabelData (ULabelList.pas:10-16), field for field. process:
   Pascal's Word widened to `long`; the truncation it replaces is never
   observable. condact: always -1 in every call this port's sources
   make and never read back downstream (defect 19.30, DEAD-CODE). */
typedef struct LabelData {
    const char *skip_label; /* includes '$'; case-sensitive key */
    long process;
    long entry;
    int is_forward;
    int condact;
} LabelData;

typedef struct LabelTable LabelTable;

LabelTable *labels_new(Arena *a);

/* PORT: AddLabel (ULabelList.pas:32-74). `label` must already include
   its '$'; compared byte-for-byte, no folding. Branches exactly as the
   Pascal:

     - no existing entry with this exact key: appended as a new slot,
       returns the new index (>= 0).
     - existing entry, was forward, `is_forward` false: updates
       process/entry/condact in place, clears is_forward, returns that
       slot's index (>= 0) - this is how a real definition resolves an
       earlier forward SKIP reference.
     - existing entry, was forward, `is_forward` true: one more forward
       reference to the same not-yet-defined label; returns that slot's
       index (>= 0) UNCHANGED, nothing is written.
     - existing entry, already real (not forward), regardless of
       `is_forward`: a second real definition. Returns -1. (A caller
       resolving a SKIP to an ALREADY-defined label does not come
       through here at all in the reference - USintactic.pas resolves
       that case directly against labels_find - so this leg is reached
       only by a genuine duplicate label definition.)

   Table full at MAX_LABELS: diag_fatal already reported, returns -2 -
   caller must stop, not re-diagnose (unlike -1 = duplicate, caller
   diagnoses). */
long labels_add(LabelTable *t, Arena *a, Diag *d, const char *label,
                 long process, long entry, int is_forward, int condact);

/* PORT: GetLabelData (ULabelList.pas:76-90). Linear scan, ignoring any
   entry still IsForward, keeping the LAST match (immaterial in
   practice - AddLabel's own duplicate rejection means at most one
   non-forward entry can ever share a key). Returns 1 and fills *out on
   a hit, 0 (out untouched) on a miss. */
int labels_find(const LabelTable *t, const char *label, LabelData *out);

/* Iteration surface for Task 7's FixForwardLabels-equivalent pass:
   slots 0..labels_count(t)-1, in AddLabel's insertion order (matches
   NextFreeLabelSlot). labels_at returns NULL for an out-of-range
   index. */
size_t labels_count(const LabelTable *t);
const LabelData *labels_at(const LabelTable *t, size_t index);

#endif /* NDRC_FRONT_LABELS_H */
