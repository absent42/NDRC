/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/constants.h - Copyright (C) 2026 Dan Gibson.

   PORT: UConstants.pas CONST block verbatim (same names, values,
   order). The VAR block is runtime option state, owned by the
   CLI/option layer. Four constants are DEAD in the reference (marked
   below); nothing here should depend on them. */
#ifndef NDRC_FRONT_CONSTANTS_H
#define NDRC_FRONT_CONSTANTS_H

#define VERSION_HI 0
#define VERSION_LO 40

#define LOC_CARRIED 254
#define LOC_WORN 253
#define LOC_NOT_CREATED 252
#define LOC_HERE 255
#define NO_WORD 255
#define MAX_FLAG_VALUE 255
#define VOCABULARY_LENGTH 5
#define MAX_DIRECTION_VOCABULARY 13     /* DEAD (19.50/section 18) */
#define MAX_CONVERTIBLE_NAME 39
#define MAX_PROCESSES 255
#define MAX_CONDACT_PARAMS 3
#define MAX_V3_DIRECTION 127            /* DEAD (19.50/section 18) */
#define MAX_BLOCKABLE_CONNECTIONS 128   /* DEAD (19.50/section 18) */
#define MAX_OBJECTS 256

#define MAX_MESSAGES_PER_TABLE 255
#define MAX_WEIGHT 63

#define MAX_PARAMETER_RANGE 255

/* Defect 19.23: at this ceiling the reference AddLabel returns the
   loop index (1024), so 'too many labels' never fires and a later
   label id is corrupted (live-verified, 1030 labels: exhausted table
   surfaces as `Invalid parameter value "1023" for condact
   PENDINGSKIP`). labels_add FATALs instead. */
#define MAX_LABELS 1024

#define NUM_CONDACTS 128
#define NUM_FAKE_CONDACTS 16
#define NUM_PREFIX_CONDACTS 10 /* DEAD (19.50/section 18) - corroborates 19.47's unimplemented prefix-condact feature */

#define MESSAGE_OPCODE 38
#define MES_OPCODE 77
#define SYSMESS_OPCODE 54
#define XMES_OPCODE 128
#define XMESSAGE_OPCODE 129
#define XPICTURE_OPCODE 130
#define PICTURE_OPCODE 84
#define XSAVE_OPCODE 131
#define SAVE_OPCODE 25
#define XLOAD_OPCODE 132
#define LOAD_OPCODE 26
#define XPLAY_OPCODE 134
#define XBEEP_OPCODE 135
#define XSPLITSCR_OPCODE 136
#define XUNDONE_OPCODE 137
#define XNEXTCLS_OPCODE 138
#define XNEXTRST_OPCODE 139
#define XSPEED_OPCODE 140
#define XDATA_OPCODE 142
#define BEEP_OPCODE 64

#define DESC_OPCODE 19
#define SKIP_OPCODE 116
#define PENDINGSKIP_OPCODE 141

#define SYNONYM_OPCODE 36
#define PREP_OPCODE 68
#define NOUN2_OPCODE 69
#define ADJECT1_OPCODE 16
#define ADVERB_OPCODE 17
#define ADJECT2_OPCODE 70
#define MES2_OPCODE (512 + 9)
/* MES2_OPCODE (521) / TOGGLECON_OPCODE (520) are compared at
   USintactic.pas:617,623 but sit outside GetCondact's reachable
   0..143-plus-220 range (defect 19.28) - dead comparison, live constants. */
#define FAKE_DEBUG_CONDACT_CODE 220 /* the fake DEBUG Condact */
#define FAKE_DEBUG_CONDACT_TEXT "DEBUG"

#define FAKE_USERPTR_CONDACT_CODE 256

#define TOGGLECON_OPCODE 520

#endif /* NDRC_FRONT_CONSTANTS_H */
