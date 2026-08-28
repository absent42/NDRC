/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/condacts.c - Copyright (C) 2026 Dan Gibson. */
#include "condacts.h"

#include <string.h>

#include "constants.h"
#include "str.h"

/* PORT: Condacts (UCondacts.pas:25-172), transcribed row for row in
   Pascal declaration order, script-diff verified element-wise against
   the Pascal source. Opcodes 120/122/124 carry the literal
   lower-case placeholder name "dumb" exactly as the source spells it;
   this is the BASE (v3=0) shape - ApplyV3Changes' two rewrites
   (opcode 122 -> INDIR, opcode 124 -> SETAT) are applied at lookup
   time via the override table below, never by mutating this array. */
static const CondactDef CONDACTS[NUM_CONDACTS + NUM_FAKE_CONDACTS] = {
    {0, 1, "AT", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 1},
    {1, 1, "NOTAT", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 1},
    {2, 1, "ATGT", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 1},
    {3, 1, "ATLT", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 1},
    {4, 1, "PRESENT", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 1},
    {5, 1, "ABSENT", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 1},
    {6, 1, "WORN", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 1},
    {7, 1, "NOTWORN", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 1},
    {8, 1, "CARRIED", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 1},
    {9, 1, "NOTCARR", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 1},
    {10, 1, "CHANCE", PARAM_PERCENT, PARAM_NONE, PARAM_NONE, 1},
    {11, 1, "ZERO", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 1},
    {12, 1, "NOTZERO", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 1},
    {13, 2, "EQ", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 1},
    {14, 2, "GT", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 1},
    {15, 2, "LT", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 1},
    {16, 1, "ADJECT1", PARAM_VOC_ADJECT, PARAM_NONE, PARAM_NONE, 1},
    {17, 1, "ADVERB", PARAM_VOC_ADVERB, PARAM_NONE, PARAM_NONE, 1},
    {18, 2, "SFX", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {19, 1, "DESC", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 0},
    {20, 0, "QUIT", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {21, 0, "END", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {22, 0, "DONE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {23, 0, "OK", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {24, 0, "ANYKEY", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {25, 1, "SAVE", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {26, 1, "LOAD", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {27, 1, "DPRINT", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {28, 1, "DISPLAY", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {29, 0, "CLS", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {30, 0, "DROPALL", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {31, 0, "AUTOG", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {32, 0, "AUTOD", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {33, 0, "AUTOW", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {34, 0, "AUTOR", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    /* PAUSE: NumParams=1 but Type1=none (UCondacts.pas:61) - the
       duration parameter is never routed through SemanticCheck's
       typed arms; analysis 17.2 footnote (*). Ported verbatim, not
       "corrected" to PARAM_VALUE. */
    {35, 1, "PAUSE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {36, 2, "SYNONYM", PARAM_VOC_VERB, PARAM_VOC_NOUN, PARAM_NONE, 0},
    {37, 1, "GOTO", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 0},
    {38, 1, "MESSAGE", PARAM_MESNO, PARAM_NONE, PARAM_NONE, 0},
    {39, 1, "REMOVE", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {40, 1, "GET", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {41, 1, "DROP", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {42, 1, "WEAR", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {43, 1, "DESTROY", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {44, 1, "CREATE", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {45, 2, "SWAP", PARAM_OBJNO, PARAM_OBJNO, PARAM_NONE, 0},
    {46, 2, "PLACE", PARAM_OBJNO, PARAM_LOCNO_, PARAM_NONE, 0},
    {47, 1, "SET", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {48, 1, "CLEAR", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {49, 2, "PLUS", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 0},
    {50, 2, "MINUS", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 0},
    {51, 2, "LET", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 0},
    {52, 0, "NEWLINE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {53, 1, "PRINT", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {54, 1, "SYSMESS", PARAM_SYSNO, PARAM_NONE, PARAM_NONE, 0},
    {55, 2, "ISAT", PARAM_OBJNO, PARAM_LOCNO_, PARAM_NONE, 1},
    {56, 1, "SETCO", PARAM_OBJNO, PARAM_NONE, PARAM_NONE, 0},
    {57, 0, "SPACE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {58, 1, "HASAT", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 1},
    {59, 1, "HASNAT", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 1},
    {60, 0, "LISTOBJ", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {61, 2, "EXTERN", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {62, 0, "RAMSAVE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {63, 1, "RAMLOAD", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {64, 2, "BEEP", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {65, 1, "PAPER", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {66, 1, "INK", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {67, 1, "BORDER", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {68, 1, "PREP", PARAM_VOC_PREP, PARAM_NONE, PARAM_NONE, 1},
    {69, 1, "NOUN2", PARAM_VOC_NOUN, PARAM_NONE, PARAM_NONE, 1},
    {70, 1, "ADJECT2", PARAM_VOC_ADJECT, PARAM_NONE, PARAM_NONE, 1},
    {71, 2, "ADD", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 0},
    {72, 2, "SUB", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 0},
    {73, 1, "PARSE", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {74, 1, "LISTAT", PARAM_LOCNO_, PARAM_NONE, PARAM_NONE, 0},
    {75, 1, "PROCESS", PARAM_PROCNO, PARAM_NONE, PARAM_NONE, 0},
    {76, 2, "SAME", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 1},
    {77, 1, "MES", PARAM_MESNO, PARAM_NONE, PARAM_NONE, 0},
    {78, 1, "WINDOW", PARAM_WINDOW, PARAM_NONE, PARAM_NONE, 0},
    {79, 2, "NOTEQ", PARAM_FLAGNO, PARAM_VALUE, PARAM_NONE, 1},
    {80, 2, "NOTSAME", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 1},
    {81, 1, "MODE", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {82, 2, "WINAT", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {83, 2, "TIME", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {84, 1, "PICTURE", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {85, 1, "DOALL", PARAM_LOCNO_, PARAM_NONE, PARAM_NONE, 0},
    {86, 2, "MOUSE", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {87, 2, "GFX", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {88, 2, "ISNOTAT", PARAM_OBJNO, PARAM_LOCNO_, PARAM_NONE, 1},
    {89, 2, "WEIGH", PARAM_OBJNO, PARAM_FLAGNO, PARAM_NONE, 0},
    {90, 2, "PUTIN", PARAM_OBJNO, PARAM_LOCNO, PARAM_NONE, 0},
    {91, 2, "TAKEOUT", PARAM_OBJNO, PARAM_LOCNO, PARAM_NONE, 0},
    {92, 0, "NEWTEXT", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {93, 2, "ABILITY", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {94, 1, "WEIGHT", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {95, 1, "RANDOM", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {96, 2, "INPUT", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {97, 0, "SAVEAT", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {98, 0, "BACKAT", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {99, 2, "PRINTAT", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {100, 0, "WHATO", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {101, 2, "CALL", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {102, 1, "PUTO", PARAM_LOCNO_, PARAM_NONE, PARAM_NONE, 0},
    {103, 0, "NOTDONE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {104, 1, "AUTOP", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 0},
    {105, 1, "AUTOT", PARAM_LOCNO, PARAM_NONE, PARAM_NONE, 0},
    {106, 1, "MOVE", PARAM_FLAGNO, PARAM_NONE, PARAM_NONE, 0},
    {107, 2, "WINSIZE", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {108, 0, "REDO", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {109, 0, "CENTRE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {110, 1, "EXIT", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {111, 0, "INKEY", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {112, 2, "BIGGER", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 1},
    {113, 2, "SMALLER", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 1},
    {114, 0, "ISDONE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 1},
    {115, 0, "ISNDONE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 1},
    {116, 1, "SKIP", PARAM_SKIP, PARAM_NONE, PARAM_NONE, 0},
    {117, 0, "RESTART", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {118, 1, "TAB", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {119, 2, "COPYOF", PARAM_OBJNO, PARAM_FLAGNO, PARAM_NONE, 0},
    {120, 0, "dumb", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {121, 2, "COPYOO", PARAM_OBJNO, PARAM_OBJNO, PARAM_NONE, 0},
    {122, 0, "dumb", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {123, 2, "COPYFO", PARAM_FLAGNO, PARAM_OBJNO, PARAM_NONE, 0},
    {124, 0, "dumb", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {125, 2, "COPYFF", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 0},
    {126, 2, "COPYBF", PARAM_FLAGNO, PARAM_FLAGNO, PARAM_NONE, 0},
    {127, 0, "RESET", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    /* Additional fake condacts */
    {128, 1, "XMES", PARAM_STRING, PARAM_NONE, PARAM_NONE, 0},
    {129, 1, "XMESSAGE", PARAM_STRING, PARAM_NONE, PARAM_NONE, 0},
    {130, 1, "XPICTURE", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {131, 1, "XSAVE", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {132, 1, "XLOAD", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {133, 1, "XPART", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {134, 1, "XPLAY", PARAM_STRING, PARAM_NONE, PARAM_NONE, 0},
    {135, 2, "XBEEP", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0},
    {136, 1, "XSPLITSCR", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {137, 0, "XUNDONE", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {138, 0, "XNEXTCLS", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {139, 0, "XNEXTRST", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
    {140, 1, "XSPEED", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {141, 1, "PENDINGSKIP", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0},
    {142, 1, "XDATA", PARAM_STRING, PARAM_NONE, PARAM_NONE, 0},
    {143, 0, "GETKEY", PARAM_NONE, PARAM_NONE, PARAM_NONE, 0},
};

/* PORT: the synthetic FAKE_DEBUG_CONDACT_CODE (220) definition -
   GetCondact special-cases the name match and GetNumParams special-
   cases the opcode, both WITHOUT ever indexing Condacts[] (17.2/17.3);
   this single static row is what both condact_lookup("DEBUG", _) and
   condact_by_opcode(220, _) hand back. */
static const CondactDef DEBUG_CONDACT = {
    FAKE_DEBUG_CONDACT_CODE, 0, FAKE_DEBUG_CONDACT_TEXT,
    PARAM_NONE, PARAM_NONE, PARAM_NONE, 0
};

/* PORT: ApplyV3Changes (UCondacts.pas:276-292) - the two rewritten
   rows, held as separate static overrides rather than mutating
   CONDACTS in place (condacts.h's doc comment explains why: no shared
   mutable state, `v3` threaded explicitly instead). */
static const CondactDef V3_OVERRIDE_122 = {
    122, 1, "INDIR", PARAM_VALUE, PARAM_NONE, PARAM_NONE, 0
};
static const CondactDef V3_OVERRIDE_124 = {
    124, 2, "SETAT", PARAM_VALUE, PARAM_VALUE, PARAM_NONE, 0
};

const CondactDef *condact_table(void)
{
    return CONDACTS;
}

size_t condact_table_len(void)
{
    return NUM_CONDACTS + NUM_FAKE_CONDACTS;
}

const CondactDef *condact_by_opcode(int opcode, int v3)
{
    if (opcode == FAKE_DEBUG_CONDACT_CODE) return &DEBUG_CONDACT;
    if (opcode < 0 || opcode >= (int)condact_table_len()) return NULL;
    if (v3 && opcode == 122) return &V3_OVERRIDE_122;
    if (v3 && opcode == 124) return &V3_OVERRIDE_124;
    return &CONDACTS[opcode];
}

const CondactDef *condact_lookup(const char *name, int v3)
{
    /* UCondacts.pas:195,197 - JUMP/DEBUG fold via ASCII-only UpperCase,
       not the AnsiUpperCase the table scan below uses; kept byte-faithful. */
    {
        /* Fixed buffer avoids needing an Arena (condact_lookup takes
           none, see condacts.h); PENDINGSKIP (11 bytes) is the longest
           legal name, so overlong input just skips to the table scan. */
        char buf[64];
        size_t len = strlen(name);
        size_t i;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)name[i];
            buf[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
        buf[len] = '\0';

        if (strcmp(buf, "JUMP") == 0) name = "SKIP";
        else if (strcmp(buf, FAKE_DEBUG_CONDACT_TEXT) == 0) return &DEBUG_CONDACT;
    }

    /* UCondacts.pas:203-215 - Pascal folds both sides with
       AnsiUpperCase (Latin-1); every table name is pure ASCII, so
       ASCII-only str_ieq is provably equivalent here and avoids an
       Arena parameter. */
    {
        size_t n = condact_table_len();
        size_t i;
        for (i = 0; i < n; i++) {
            const CondactDef *def = &CONDACTS[i];
            if (v3 && i == 122) def = &V3_OVERRIDE_122;
            else if (v3 && i == 124) def = &V3_OVERRIDE_124;

            if (str_ieq(def->name, name)) return def;
        }
    }
    return NULL;
}

/* PORT: SemanticVocabularyCheck (UCondacts.pas:231-243). `word` is the
   parameter's raw source text; `_` is exempt. Truncates to 5 bytes,
   THEN upper-cases (str_upper_latin1) - the error message uses this
   truncated-and-uppercased-but-NOT-FixSpanishChars-folded text, since
   FixSpanishChars only ever runs inside voctree_lookup's own internal
   canonicalisation, never observable back to this caller. */
static const char *semantic_vocabulary_check(Arena *a, const VocTree *voctree,
                                              VocType type, const char *word)
{
    char truncated[6];
    char *upper;
    size_t len;
    VocEntry entry;
    Str *msg;

    if (strcmp(word, "_") == 0) return NULL;

    len = strlen(word);
    if (len > 5) len = 5;
    memcpy(truncated, word, len);
    truncated[len] = '\0';
    upper = str_upper_latin1(a, truncated);

    if (voctree_lookup(voctree, a, upper, type, &entry)) return NULL;

    msg = str_new(a);
    str_append(msg, "Word not defined in vocabulary or it has an "
                     "unexpected word type : ");
    str_append(msg, upper);
    return str_cstr(msg);
}

const char *condact_semantic_check(Arena *a, int opcode, int v3,
    int param_num, int param_value, const char *param_as_string,
    const VocTree *voctree, const MessageList *messages)
{
    const CondactDef *def = condact_by_opcode(opcode, v3);
    ParamType expected;
    Str *msg;

    if (def == NULL) return NULL; /* unreachable on any traversed path */

    /* PORT: GetParamType (UCondacts.pas:224-229) - ParamNum=1->Type1,
       =2->Type2, anything ELSE (including 0, or > 3) ->Type3, with NO
       bounds check, reproduced verbatim. */
    if (param_num == 1) expected = def->type1;
    else if (param_num == 2) expected = def->type2;
    else expected = def->type3;

    switch (expected) {
    case PARAM_LOCNO:
        if (param_value >= messages->ltx_count) {
            msg = str_new(a);
            str_appendf(msg, "Location %d does not exist", param_value);
            return str_cstr(msg);
        }
        return NULL;
    case PARAM_OBJNO:
        if (param_value >= messages->otx_count) {
            msg = str_new(a);
            str_appendf(msg, "Object %d does not exist", param_value);
            return str_cstr(msg);
        }
        return NULL;
    case PARAM_FLAGNO:
        return NULL;
    case PARAM_SYSNO:
        if (param_value >= messages->stx_count) {
            msg = str_new(a);
            str_appendf(msg, "System message %d does not exist", param_value);
            return str_cstr(msg);
        }
        return NULL;
    case PARAM_MESNO:
        if (param_value >= messages->mtx_count) {
            msg = str_new(a);
            str_appendf(msg, "Message %d does not exist", param_value);
            return str_cstr(msg);
        }
        return NULL;
    case PARAM_PROCNO:
        return NULL; /* forward references are legal - deliberately unchecked */
    case PARAM_VALUE:
        return NULL;
    case PARAM_LOCNO_:
        /* PORT: defect 19.39 - "Location  <n>" has TWO spaces. */
        if (param_value >= messages->ltx_count && param_value < 252) {
            msg = str_new(a);
            str_appendf(msg, "Location  %d does not exist", param_value);
            return str_cstr(msg);
        }
        return NULL;
    case PARAM_PERCENT:
        if (param_value >= 100 || param_value == 0) {
            return "Invalid percent value, must be in the 1-99 range";
        }
        return NULL;
    case PARAM_VOC_VERB: {
        const char *r = semantic_vocabulary_check(a, voctree, VOC_VERB,
                                                    param_as_string);
        if (r != NULL) {
            /* PORT: UCondacts.pas:263 - only SYNONYM has a
               vocabularyVerb parameter; on a VERB miss, retry as a
               NOUN (convertible-noun fallback), semantically only. */
            r = semantic_vocabulary_check(a, voctree, VOC_NOUN,
                                           param_as_string);
        }
        return r;
    }
    case PARAM_VOC_NOUN:
        return semantic_vocabulary_check(a, voctree, VOC_NOUN, param_as_string);
    case PARAM_VOC_PREP:
        return semantic_vocabulary_check(a, voctree, VOC_PREPOSITION,
                                          param_as_string);
    case PARAM_VOC_ADVERB:
        return semantic_vocabulary_check(a, voctree, VOC_ADVERB, param_as_string);
    case PARAM_VOC_ADJECT:
        return semantic_vocabulary_check(a, voctree, VOC_ADJECT, param_as_string);
    case PARAM_SKIP:
        return NULL;
    case PARAM_STRING:
        return NULL;
    case PARAM_WINDOW:
        if (param_value > 7) {
            return "Invalid window number, must be in the 0-7 range";
        }
        return NULL;
    case PARAM_NONE:
    case PARAM_BITNO: /* dead, defect 19.48 - falls to the unconditional pass */
    default:
        return NULL;
    }
}
