/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/condacts.h - Copyright (C) 2026 Dan Gibson.

   PORT: UCondacts.pas - the 144-row condact table (UCondacts.pas:
   25-172), its lookup, and SemanticCheck's per-parameter-type
   validation (UCondacts.pas:246-274).

   The Pascal table is a CONST array ApplyV3Changes mutates in place
   (opcodes 122, 124) for -v3. This port never mutates shared state:
   `v3` threads as a parameter, and the two rewritten rows are static
   overrides substituted at lookup time - identical to the Pascal
   in-place mutation (17.1); see condacts.c. */
#ifndef NDRC_FRONT_CONDACTS_H
#define NDRC_FRONT_CONDACTS_H

#include <stddef.h>

#include "arena.h"
#include "messagelist.h"
#include "voctree.h"

/* PORT: TParamType (UCondacts.pas:8-13), member for member, same
   declaration order. PARAM_ names avoid colliding with C keywords
   (none/window) and with the `string_`/`locno_` Pascal-side trailing
   underscores some idents already carry for the same reason. */
typedef enum ParamType {
    PARAM_NONE,
    PARAM_LOCNO,
    PARAM_OBJNO,
    PARAM_FLAGNO,
    PARAM_SYSNO,
    PARAM_MESNO,
    PARAM_PROCNO,
    PARAM_VALUE,
    PARAM_LOCNO_,
    PARAM_PERCENT,
    PARAM_VOC_VERB,
    PARAM_VOC_NOUN,
    PARAM_VOC_PREP,
    PARAM_VOC_ADVERB,
    PARAM_VOC_ADJECT,
    PARAM_SKIP,
    PARAM_STRING,
    PARAM_WINDOW,   /* 0-7 */
    PARAM_BITNO     /* 0-15; declared, never assigned by any table row -
                        entirely dead, defect 19.48. Ported for fidelity
                        to the Pascal enum's member list. */
} ParamType;

/* PORT: TCondact (UCondacts.pas:15-22), plus an explicit `opcode`
   field the Pascal record does not carry (there it is the array
   INDEX, implicit). An explicit field is required here because a -v3
   lookup can hand back a definition that is NOT stored at its "home"
   index in the base table (the two ApplyV3Changes overrides) - see
   condact_by_opcode. */
typedef struct CondactDef {
    int opcode;
    int num_params;      /* NumParams */
    const char *name;    /* Condact */
    ParamType type1, type2, type3;
    int can_be_jump;      /* CanBeJump */
} CondactDef;

/* The base (v3=0) 144-row table in Pascal declaration order, opcodes
   0..143 (NUM_CONDACTS + NUM_FAKE_CONDACTS). Exposed for the rule-0.2
   script-diff and for tests; not itself part of the lookup API a
   parser calls day to day (condact_lookup/condact_by_opcode are). */
const CondactDef *condact_table(void);
size_t condact_table_len(void); /* NUM_CONDACTS + NUM_FAKE_CONDACTS = 144 */

/* PORT: GetCondact (UCondacts.pas:191-216). Three-step lookup: JUMP
   aliases to SKIP (ASCII-only fold), DEBUG short-circuits to a
   synthetic definition (same fold, checked first), else a
   case-insensitive AnsiUpperCase linear scan of 0..143, first match
   wins. Opcodes 120/122/124 carry the literal "dumb" placeholder
   (UCondacts.pas:146,148,150), so "DUMB" resolves to opcode 120
   (17.3). `v3` substitutes ApplyV3Changes' two rewritten rows
   (122->INDIR, 124->SETAT) for the placeholders. NULL on no match
   (GetCondact's -1); no Arena - nothing here is per-compile state. */
const CondactDef *condact_lookup(const char *name, int v3);

/* PORT: GetNumParams (UCondacts.pas:218-222) folded with GetParamType
   (UCondacts.pas:224-229). Opcode FAKE_DEBUG_CONDACT_CODE (220)
   returns the same synthetic zero-parameter definition
   condact_lookup("DEBUG", _) does; `v3` substitutes the same two
   ApplyV3Changes overrides condact_lookup does. Returns NULL for any
   opcode outside 0..143 other than 220. Pascal has no bounds check
   (defect 19.49, dead there); a C OOB read is a real overrun, so this
   guard is the memory-safety carve-out. */
const CondactDef *condact_by_opcode(int opcode, int v3);

/* PORT: SemanticCheck (UCondacts.pas:246-274). Looks up the
   parameter's declared type via condact_by_opcode/GetParamType's
   ParamNum rule (1->Type1, 2->Type2, else->Type3, no bounds check)
   and validates `param_value` against it. `voctree`/`messages` supply
   GetVocabulary and the LTX/OTX/STX/MTX counts - the SECTION counts
   frozen after parse, NOT the live table length (an auto-inserted
   overflow/inline message, 24.5, is still a semantic error because
   these counts don't grow). Returns NULL for no error, else the
   verbatim message text (the `locno_` double-space QUIRK, 19.39,
   included; see condacts.c). */
const char *condact_semantic_check(Arena *a, int opcode, int v3,
    int param_num, int param_value, const char *param_as_string,
    const VocTree *voctree, const MessageList *messages);

#endif /* NDRC_FRONT_CONDACTS_H */
