/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/model.h - Adventure model, built from the JSON DOM.
   Copyright (C) 2026 Dan Gibson.

   Schema per analysis S16 and the measured inventory of real JSON files,
   cross-checked against the JSON writer (UJSONExport.pas GenerateJSON):
   top-level keys settings, symbols, externs, vocabulary, object_data,
   connections, messages, sysmess, locations, objects, xmessages,
   other_strings, processes.

   `symbols` is never read (DRB does not consume it - drb.php has no
   reference to $adventure->symbols anywhere). `externs` IS loaded and
   supported (embedded externs): each entry's FilePath is a "path|TYPE"
   string (UJSONExport.pas:304-312 emits only {"FilePath":"<string>"}),
   split at parse time into the ExternEntry below rather than at emit
   time as DRB's generateExterns does inline (drb.php:109-113) - a
   missing "|TYPE" defaults file_type to "EXTERN" (drb.php:111, a
   dormant back-compat arm for old JSON files). See emit.h's
   emit_externs for the emission side. `xmessages` IS loaded and
   supported - see the xmessage_* fields below. `other_strings` IS
   loaded (the XDATA rewrite reads other_strings[Param1]->Text,
   drb.php:961) but is exempt from the replaceChars/checkStrings
   passes, which name only their four/five tables by name.

   Param1 is optional like Param2..4: UJSONExport.pas only emits
   ParamN/IndirectionN for N < NumParams, so a zero-parameter condact
   never carries a Param1 key - treating it as required would reject
   most real DAAD games. */
#ifndef NDRC_MODEL_H
#define NDRC_MODEL_H

#include "arena.h"
#include "diag.h"
#include "json.h"
#include "str.h"
#include "vec.h"

/* PORT: drb.php:107-113's split of one extern's FilePath ("path|TYPE",
   a missing "|TYPE" defaulting to "EXTERN"), pre-computed here at JSON-
   parse time (model.c's split_extern_file_path) rather than at emit
   time. file_type is one of "EXTERN", "SFX" or "INT" when valid, or any
   other string when the source JSON names an invalid type - emit_externs
   (emit.h) is what reports that as a fatal error, matching drb.php:124's
   default switch arm running at emission, not parse, time. */
typedef struct ExternEntry {
    const char *file_path;
    const char *file_type;
} ExternEntry;

VEC_DECLARE(ExternEntry, ExternEntry *)

typedef struct {
    long Value;
    Str *Text;
    Str *originalText;         /* set by the replaceEscapeChars pass */
} Message;

VEC_DECLARE(Message, Message *)
VEC_DECLARE(MsgTable, Vec_Message *)

typedef struct {
    long Value, Noun, Adjective, Container, Wearable, Flags, Weight, InitialyAt;
} ObjectData;

VEC_DECLARE(ObjectData, ObjectData *)

typedef struct {
    long FromLoc, ToLoc, Direction;
} Connection;

VEC_DECLARE(Connection, Connection *)

typedef struct {
    const char *VocWord;
    long Value, VocType;
} VocWordEntry;

VEC_DECLARE(VocWordEntry, VocWordEntry *)

typedef struct {
    long Opcode, NumParams, Param1, Param2, Param3, Param4;
    long Indirection1, Indirection2;
    const char *Condact;
    int DurationAdjusted;
} Condact;

VEC_DECLARE(Condact, Condact *)

typedef struct {
    const char *Entry;
    long Verb, Noun;
    Vec_Condact *condacts;
} ProcEntry;

VEC_DECLARE(ProcEntry, ProcEntry *)

typedef struct {
    long Value;
    Vec_ProcEntry *entries;
} Process;

VEC_DECLARE(Process, Process *)

typedef struct Adventure {
    int classic_mode, debug_mode, v3code, maluva_used;
    Vec_ExternEntry *externs;
    Vec_Message *messages, *sysmess, *locations, *objects, *xmessages;
    Vec_Message *other_strings;   /* consumed by the XDATA rewrite, drb.php:961 */
    Vec_ObjectData *object_data;
    Vec_Connection *connections;
    Vec_VocWordEntry *vocabulary;      /* in JSON (sorted) order - do NOT reorder */
    Vec_Process *processes;
    long extvec[13];

    /* Set by emit_xmessages (back/emit_xmb.c), PORT: generateXMessages
       drb.php:449-524. Untouched (NULL/0) when adv->xmessages is empty -
       emit_xmessages is only ever called when vec_len_Message(adv->xmessages)>0
       (main.c's own guard, drb.php:1920). */
    long *xmessage_offsets;   /* one per xmessage, drb.php:501 xMessageOffsets */
    long xmessage_size;       /* drb.php:523 xMessageSize */
    long xmessage_padding;    /* drb.php:498 paddingSize (0 unless the
                                  pad-and-continue branch ran) */
    long xmessage_max_k;      /* drb.php:454 maxFileSizeForXMessages */
} Adventure;

/* Builds an Adventure from a parsed JSON DOM, then runs DRB's three input
   passes (drb.php:1783-1789): replaceEscapeChars, checkStrings, and
   zeroing extvec. Every diagnostic - missing/mistyped JSON field, the
   externs gate, or an invalid character caught by
   checkStrings - is reported through d via diag_fatal, and this function
   returns NULL the moment the first one is reported (DRC halts at its
   first error; see diag.h). Returns the built Adventure on success. */
Adventure *model_from_json(Arena *a, Diag *d, const JsonValue *root);

#endif /* NDRC_MODEL_H */
