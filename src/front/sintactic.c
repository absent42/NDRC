/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/sintactic.c - Copyright (C) 2026 Dan Gibson.

   PORT: USintactic.pas, function for function, Pascal lines cited at
   each site. SyntaxError Halt(1)s in the reference; this port reports
   via diag and longjmps out with the same exit class. Unit globals
   are file-scope statics. */
#include "sintactic.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "str.h"

#include "condacts.h"
#include "constants.h"
#include "expr.h"
#include "include.h"
#include "lex_tokens.h"

/* PORT: Pascal's MAXLONGINT sentinel, the "no value / not found"
   marker threaded through GetSymbolValue/GetIdentifierValue/
   ExtractValue and the token IntVal field (analysis 15.3). Same value
   as tokenlist.h's TOKEN_NO_VALUE; named as the Pascal names it so
   the ported control flow reads one-to-one. */
#define MAXLONGINT 2147483647L

/* ---- USintactic.pas unit globals (25-31, 15-19), file-scope ---- */

static Arena *g_arena;
static Diag *g_diag;
static const FrontOptions *g_opts;
static jmp_buf g_jmp;

static const char *current_text;   /* CurrentText */
static long current_int_val;       /* CurrentIntVal */
static int current_token_id;       /* CurrentTokenID (Word) */
static long curr_lineno;           /* CurrLineno */
static int curr_colno;             /* CurrColno (Word) */
static Token *cur;                 /* CurrTokenPTR */

static int classic_mode;           /* ClassicMode */
static int debug_mode;             /* DebugMode */
static int maluva_used;            /* MaluvaUsed - set only by Task 7's
                                      XPICTURE branch; carried here as
                                      the unit global it is */
static const char *g_target;       /* Target */
static const char *g_subtarget;    /* Subtarget */
static unsigned global_nested_ifdef_count; /* GlobalNestedIfdefCount (Word) */

/* ---- the driven units' globals (USES clause), file-scope ---- */

static SymbolList *g_symbols;       /* USymbolList.SymbolList */
static VocTree *g_voctree;          /* UVocabularyTree.VocabularyTree */
static MessageList *g_messages;     /* UMessageList's six lists+counts */
static ConnectionList *g_connections; /* UConnections.Connections */
static ObjectList *g_objects;       /* UObjects.ObjectList */
static ProcessTable *g_processes;   /* UProcess.Processes */
static long g_last_process;         /* UProcess.LastProcess */
static LabelTable *g_labels;        /* ULabelList.LabelList */
static CTLExternList *g_externs;    /* UCTLExtern.CTLExternList */

/* ---- error reporting ---- */

static char *format_body(const char *fmt, va_list ap)
{
    char *body;
    va_list copy;
    int n;

    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    body = arena_alloc(g_arena, (size_t)(n < 0 ? 0 : n) + 1u);
    vsnprintf(body, (size_t)(n < 0 ? 0 : n) + 1u, fmt, ap);
    return body;
}

/* PORT: SyntaxError (USintactic.pas:34-40) - maps CurrLineno through
   GetIncludeData so the ORIGINAL file/line is reported (include.h's
   remap helper re-points diag's source name on a map hit), prints the
   `line:col:file: msg.` shape, and halts - here, longjmps out of the
   parse with exit class 1 already recorded. */
static _Noreturn void syntax_error(const char *fmt, ...)
{
    va_list ap;
    char *body;
    long mapped;

    va_start(ap, fmt);
    body = format_body(fmt, ap);
    va_end(ap);
    mapped = include_remap_for_diag(g_diag, curr_lineno);
    diag_syntax_error(g_diag, (int)mapped, curr_colno, "%s", body);
    longjmp(g_jmp, 1);
}

/* PORT: Warning (USintactic.pas:42-47) - same shape prefixed
   `Warning: `, does not halt. */
static void warning(const char *fmt, ...)
{
    va_list ap;
    char *body;
    long mapped;

    va_start(ap, fmt);
    body = format_body(fmt, ap);
    va_end(ap);
    mapped = include_remap_for_diag(g_diag, curr_lineno);
    diag_warn(g_diag, (int)mapped, curr_colno, "%s", body);
}

/* Fatal (exit class 2) escape, for the guards that stand where the
   reference dies with an unhandled FPC runtime error whose crash dump
   is not worth reproducing (the two expression range checks, the
   semantic byte-range check, #hex's conversion) or where continuing
   would be undefined behaviour in C (the token-stream and fopen
   guards). Every one of them names the reference behaviour in its own
   text. Reports via diag_fatal then longjmps out of the parse. */
static _Noreturn void fatal_jmp(const char *fmt, ...)
{
    va_list ap;
    char *body;

    va_start(ap, fmt);
    body = format_body(fmt, ap);
    va_end(ap);
    diag_fatal(g_diag, "%s", body);
    longjmp(g_jmp, 2);
}

/* Escape for a guard that has ALREADY diagnosed (labels_add's -2,
   processtable_at's NULL): no second diagnostic, just stop. */
static _Noreturn void stop_already_diagnosed(void)
{
    longjmp(g_jmp, 2);
}

/* PORT NOTE: LexerError (USintactic.pas:49-54) is ported at its C
   call site, lex.c's rule-43 action (Task 3), which now threads the
   include map exactly as SyntaxError does - see lex.c. Nothing calls
   it from this file (in the reference the LEXER calls it, and lexing
   is over before Sintactic runs). */

/* ---- small helpers ---- */

/* Copy(S, 2, Length(S)-2) - strip the two string delimiters. */
static const char *strip_quotes(const char *s)
{
    size_t n = strlen(s);
    if (n < 2) return "";
    return arena_strndup(g_arena, s + 1, n - 2);
}

/* Copy(S, 1, VOCABULARY_LENGTH) - the 5-byte vocabulary truncation. */
static const char *copy_voc(const char *s)
{
    if (strlen(s) <= VOCABULARY_LENGTH) return s;
    return arena_strndup(g_arena, s, VOCABULARY_LENGTH);
}

static int file_exists(const char *name)
{
    FILE *f = fopen(name, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* PORT: GetSymbolValue's MAXLONGINT-sentinel contract (USymbolList.
   pas:48-54) over symbols.h's found/not-found boolean surface: a miss
   reads back as MAXLONGINT, and so does a symbol genuinely stored
   with that value (analysis 22.1: legal to store, reads back as
   undefined everywhere thereafter) - both fall out of returning the
   stored value unchanged. */
static long get_symbol_value(const char *name)
{
    long v;
    if (symbols_lookup(g_symbols, g_arena, name, &v)) return v;
    return MAXLONGINT;
}

/* ---- forward declarations (Scan's directive cases recurse) ---- */

static void scan(void);

/* ---- expression / value extraction ---- */

/* PORT: GetIdentifierValue (USintactic.pas:98-106). */
static long get_identifier_value(void)
{
    if (current_token_id == T_NUMBER) return current_int_val;
    return get_symbol_value(current_text);
}

/* PORT: GetExpressionValue (USintactic.pas:108-140) - the CALL SITE
   only; expr.h holds the TFPExpressionParser port itself (the scanner,
   the recursive descent, the evaluator and its exception texts). What
   lives here is everything the reference does AROUND that engine, in
   its order: the ShortString truncation, the quote strip, the two
   error shells, trunc() of a float result, and the Longint store.

   Symbol registration is the engine's job per call (it walks the live
   SymbolList exactly as USintactic.pas:118-123 does), so the state
   this passes is simply whatever the parse has accumulated so far. */
static long get_expression_value(void)
{
    char aux[256];        /* AuxStr : ShortString - 255 bytes, no more */
    size_t n;
    ExprResult r;
    int64_t v;

    /* USintactic.pas:124 - `AuxStr := CurrentText` truncates at 255
       bytes (defect 19.32). */
    n = strlen(current_text);
    if (n > 255) n = 255;
    memcpy(aux, current_text, n);

    /* USintactic.pas:125 - Copy(AuxStr, 2, length(AuxStr)-2) strips one
       byte from EACH end on the assumption both are quotes. On a
       truncated operand the byte taken off the tail is the 255th
       EXPRESSION character, not the closing quote (the truncation ate
       that already), so the engine silently sees a SHORTER expression:
       probe P23's 256-byte operand ending `1+29` compiles to 3, not 30.
       A Pascal Copy count below 1 yields the empty string. */
    if (n >= 2) {
        memmove(aux, aux + 1, n - 2);
        n -= 2;
    } else {
        n = 0;
    }
    aux[n] = '\0';

    expr_evaluate(aux, g_symbols, g_arena, &r);

    /* USintactic.pas:130 - the inner TRY's handler. r.msg is the engine
       text alone; the shell is composed here and SyntaxError appends
       the period. */
    if (r.kind == EXPR_FAIL)
        syntax_error("Invalid expression \"%s\": %s", aux, r.msg);

    /* USintactic.pas:135-137 - result typing, in the reference's own
       IF/IF/ELSE order. */
    if (r.kind == EXPR_FLOAT) {
        /* trunc() sits outside both TRY frames: an out-of-Int64 Double dies
           uncaught (EInvalidOp, exit 217, measured 2026-08-27 on '1e19'). The
           C cast is UB for the same values, so check first; in range it
           truncates exactly like trunc(). */
        if (!(r.fval >= -9223372036854775808.0 &&
              r.fval < 9223372036854775808.0))
            fatal_jmp("expression result %.17g is outside the Int64 range "
                      "the reference truncates into (the reference crashes "
                      "with an unhandled EInvalidOp, exit 217, here)",
                      r.fval);
        v = (int64_t)r.fval;
    } else if (r.kind == EXPR_INT) {
        v = r.ival;
    } else {
        /* USintactic.pas:137 - note there are NO quotes around the text
           in this shell, unlike the one above (probe P36). */
        syntax_error("Expression %s returned a non numeric value", aux);
    }

    /* Defect 19.55: USintactic.pas:135-136 store Int64 into a Longint
       under {$R+} (line 3); out-of-32-bit values raise an uncaught
       ERangeError, exit 217 (live-confirmed 2026-08-27, "5000000000"/"1e15"). */
    if (v < -2147483647LL - 1 || v > 2147483647LL)
        fatal_jmp("expression result %lld is outside the 32-bit range the "
                  "reference stores (the reference crashes with "
                  "ERangeError, exit 217, here)", (long long)v);

    /* A value of exactly 2147483647 flows out unchanged: it collides
       with the MAXLONGINT "no value" sentinel and is misdiagnosed by
       every caller (defect 19.56, probes P46-P48). */
    return (long)v;
}

/* PORT: ExtractValue (USintactic.pas:142-153). `symbol` NULL or empty
   selects the Pascal default parameter '' (the #db/#dw call sites);
   non-empty is the #define call site. */
static long extract_value(const char *symbol)
{
    long result;

    if (current_token_id == T_STRING) result = get_expression_value();
    else if (current_token_id == T_NUMBER || current_token_id == T_IDENTIFIER)
        result = get_identifier_value();
    else result = MAXLONGINT;
    /* Defect 19.56, REPRODUCED: this leg is live now that
       get_expression_value evaluates for real - it fires when an
       expression EVALUATES to MAXLONGINT, which is indistinguishable
       from the "no value" sentinel every other producer here uses.
       CurrentText still carries its own quotes, so the text doubles
       them (probe P47: `#define BAD "2147483647"` reports
       `""2147483647"" is not a valid expression.`, exit 1). */
    if (result == MAXLONGINT && current_token_id == T_STRING)
        syntax_error("\"%s\" is not a valid expression", current_text);
    if (result == MAXLONGINT) {
        if (symbol != NULL && symbol[0] != '\0')
            syntax_error("Value for symbol \"%s\" is not valid: \"%s\"",
                          symbol, current_text);
        else
            syntax_error("\"%s\" is not defined. Check DB/DW value.",
                          current_text);
    }
    return result;
}

/* ---- immediate directives ---- */

/* PORT: ParseDefine (USintactic.pas:155-165). */
static void parse_define(void)
{
    const char *symbol;
    long value;

    scan();
    if (current_token_id != T_IDENTIFIER)
        syntax_error("Identifier expected after #define");
    symbol = current_text;
    scan();
    value = extract_value(symbol);
    if (!symbols_add(g_symbols, g_arena, g_diag, symbol, value))
        syntax_error("\"%s\" already defined", symbol);
}

/* PORT: getMaluvaFilename (USintactic.pas:167-172) + defect 19.26:
   `Result` is assigned only for C64/CP4/PCW; for every other target
   FPC's hidden Result var parameter aliases the caller's own
   FileName, which still holds the literal "MALUVA", so the
   deterministic observed behaviour is "FileName unchanged" - the
   subsequent existence check then fails with `Extern file "MALUVA"
   not found` (live-verified per the catalogue entry). Reproduced by
   returning `current` (the caller's value) on the fall-through. */
static const char *get_maluva_filename(const char *current)
{
    if (strcmp(g_target, "C64") == 0) return "MLV_C64.BIN";
    if (strcmp(g_target, "CP4") == 0) return "MLV_CP4.BIN";
    if (strcmp(g_target, "PCW") == 0) return "MLV_PCW.BIN";
    return current;
}

/* PORT: ParseExtern (USintactic.pas:174-184). `extern_type` is the
   literal "EXTERN", "INT" or "SFX". The success line's apostrophe
   right after the type is the reference's own (defect 19.41,
   live-verified: `#EXTERN' "ext.bin" processed.`). */
static void parse_extern(const char *extern_type)
{
    const char *filename;

    scan();
    if (current_token_id != T_STRING)
        syntax_error("Included extern file should be in between quotes");
    filename = strip_quotes(current_text);
    if (strcmp(filename, "MALUVA") == 0)
        filename = get_maluva_filename(filename);
    if (!file_exists(filename))
        syntax_error("Extern file \"%s\" not found", filename);
    diag_note(g_diag, "#%s' \"%s\" processed.", extern_type, filename);
    ctlextern_add(g_externs, g_arena, filename, extern_type);
}

/* PORT: ParseEcho (USintactic.pas:187-192). Unconditional stdout-side
   output (not verbose-gated). */
static void parse_echo(void)
{
    scan();
    if (current_token_id != T_STRING)
        syntax_error("Invalid string for #echo");
    diag_note(g_diag, "%s", strip_quotes(current_text));
}

/* PORT: SkipBlock (USintactic.pas:194-216). Walks raw tokens WITHOUT
   executing directives, tracking a local depth; a nested #ifdef/
   #ifndef deepens it, #endif closes one level, and #else exits only
   at depth 1 (the ELSE of the very conditional that started the
   skip). If the exit token was #endif the cursor REWINDS one token so
   the normal path re-scans it and decrements
   GlobalNestedIfdefCount naturally; if it was #else the cursor stays
   on it (after an #else is the same as after the #ifdef). Mutates
   CurrentTokenID as the Pascal does (the caller's Scan() recursion
   reads it). */
static void skip_block(void)
{
    unsigned nested_ifdef_count = 1;
    Token *previous = cur;

    while (cur != NULL && nested_ifdef_count > 0) {
        previous = cur;
        cur = cur->next;
        if (cur != NULL) {
            current_token_id = cur->id;
            if (current_token_id == T_IFDEF || current_token_id == T_IFNDEF)
                nested_ifdef_count = nested_ifdef_count + 1;
            else if (current_token_id == T_ENDIF)
                nested_ifdef_count = nested_ifdef_count - 1;
            else if (current_token_id == T_ELSE && nested_ifdef_count == 1)
                nested_ifdef_count = 0;
        }
    }
    if (cur == NULL)
        syntax_error("Unexpected end of file. #ifdef/#ifndef couldn't find #endif");
    if (current_token_id == T_ENDIF) cur = previous;
}

/* PORT NOTE: UnScan (USintactic.pas:218-224) is NOT ported. It is
   dead code with no caller anywhere in the reference (defect 19.30),
   and it is the ONLY reader of the token list's `Previous` back-link,
   which Task 3's pinned Token struct (tokenlist.h) deliberately
   dropped for exactly that reason - the function is unimplementable
   against the C token stream and unreachable in the Pascal. Its error
   text (`UnScan() called when there is no previous token`) dies with
   it. */

/* PORT: Scan (USintactic.pas:226-282), the single token-consumption
   point. A non-directive token loads CurrentText/CurrentIntVal/
   CurrLineno/CurrColno and returns; an immediate directive (the
   eleven-member set at line 234) executes and Scan recurses to
   deliver the next non-directive token. Note the directive branch
   does NOT update the position globals - errors raised from inside a
   directive that consumed no further token report at the LAST
   non-directive token's position, as the reference does. */
static void scan(void)
{
    /* PORTED-DORMANT (defect 19.25): this guard tests BEFORE the
       advance, so in the reference it can only fire if a previous
       Scan left the cursor nil - which that Scan's own nil deref
       would already have crashed on. Masked in practice by the
       mandatory /END. */
    if (cur == NULL) syntax_error("Unexpected end of file");
    cur = cur->next;
    if (cur == NULL) {
        /* PORT NOTE (memory-safety guard, FIDELITY POLICY exception,
           defect 19.25): the reference reads CurrTokenPTR^.TokenID
           through nil here and dies with an FPC runtime error, not a
           diagnostic; a C nil deref is undefined behaviour, so this
           guard FATALs instead. Unreachable behind CheckEND's
           mandatory /END (the parse stops AT /END). */
        fatal_jmp("token stream ended unexpectedly (no /END reached)");
    }
    current_token_id = cur->id;

    switch (current_token_id) {
    case T_DEFINE: case T_IFDEF: case T_IFNDEF: case T_ENDIF: case T_ELSE:
    case T_ECHO: case T_INT: case T_SFX: case T_EXTERN: case T_DEBUG:
    case T_CLASSIC:
        break; /* immediate directive - handled below (line 234's set) */
    default:
        current_text = cur->text;
        current_int_val = cur->value;
        curr_lineno = cur->line;
        curr_colno = cur->col;
        return;
    }

    switch (current_token_id) { /* USintactic.pas:245-279 */
    case T_DEFINE: parse_define(); break;
    case T_ECHO: parse_echo(); break;
    case T_EXTERN: parse_extern("EXTERN"); break;
    case T_INT: parse_extern("INT"); break;
    case T_SFX: parse_extern("SFX"); break;
    case T_CLASSIC: classic_mode = 1; break;
    case T_DEBUG: debug_mode = 1; break;
    case T_ENDIF:
        if (global_nested_ifdef_count == 0)
            syntax_error("#endif without #ifdef/#ifndef");
        global_nested_ifdef_count = global_nested_ifdef_count - 1;
        break;
    case T_ELSE:
        /* Reached only from inside a TRUE #if(n)def branch: skip the
           else-part. The count is NOT decremented here - SkipBlock
           stops just before the #endif, which the recursion re-scans
           and decrements. */
        if (global_nested_ifdef_count == 0)
            syntax_error("#else without #ifdef/#ifndef");
        skip_block();
        break;
    case T_IFDEF: case T_IFNDEF: {
        const char *my_define;
        int evaluation;

        if (cur->next == NULL)
            syntax_error("Unexpected end of file just after #ifdef/#ifndef");
        cur = cur->next;
        if (cur->id != T_STRING)
            syntax_error("Invalid #ifdef/#ifndef label, please include the "
                          "label or expression in betwween quotes");
        /* Evaluation is EXISTENCE ONLY (defect 19.31's message
           advertises expressions that are never evaluated). */
        my_define = strip_quotes(cur->text);
        evaluation = get_symbol_value(my_define) != MAXLONGINT;
        if (current_token_id == T_IFNDEF) evaluation = !evaluation;
        global_nested_ifdef_count = global_nested_ifdef_count + 1;
        if (!evaluation) skip_block();
        break;
    }
    default: break;
    }
    scan(); /* then scan again (line 280) */
}

/* ---- sections ---- */

/* PORT: ParseCTL (USintactic.pas:288-297). The raw token id glued
   onto the failure text with no separator is the reference's own
   debug leftover (defect 19.33: `/VOC expected271`). */
static void parse_ctl(void)
{
    scan();
    if (current_token_id != T_SECTION_CTL) syntax_error("/CTL expected");
    do {
        scan();
        if (current_token_id == T_UNDERSCORE) { /* nothing */ }
        else if (current_token_id != T_SECTION_VOC)
            syntax_error("/VOC expected%d", current_token_id);
    } while (current_token_id != T_SECTION_VOC);
}

/* PORT: ParseNewWord (USintactic.pas:299-322). */
static void parse_new_word(void)
{
    long value;
    const char *the_word;
    VocType the_type;
    VocEntry aux;

    the_word = copy_voc(current_text);
    if (voctree_lookup(g_voctree, g_arena, the_word, VOC_ANY, &aux))
        syntax_error("Word \"%s\" already defined", the_word);
    scan();
    if (current_token_id == T_NUMBER || current_token_id == T_IDENTIFIER)
        value = get_identifier_value();
    else
        syntax_error("Number or Identifier expected");
    if (value == MAXLONGINT)
        syntax_error("\"%s\" is not defined", current_text);
    /* NO range check on value - defect 19.45, reproduced. */
    scan();
    {
        const char *up = str_upper_latin1(g_arena, current_text);
        if (strcmp(up, "VERB") == 0) the_type = VOC_VERB;
        else if (strcmp(up, "NOUN") == 0) the_type = VOC_NOUN;
        else if (strcmp(up, "ADJECTIVE") == 0) the_type = VOC_ADJECT;
        else if (strcmp(up, "PRONOUN") == 0) the_type = VOC_PRONOUN;
        else if (strcmp(up, "CONJUGATION") == 0) the_type = VOC_CONJUGATION;
        else if (strcmp(up, "PREPOSITION") == 0) the_type = VOC_PREPOSITION;
        else if (strcmp(up, "ADVERB") == 0) the_type = VOC_ADVERB;
        else syntax_error("\"%s\" is not a valid vocabulary word type",
                           current_text);
    }
    if (!voctree_add(g_voctree, g_arena, g_diag, g_symbols, the_word, value,
                      the_type))
        syntax_error("Vocabulary word already exists or \"_VOC_%s\" already "
                      "defined", the_word);
}

/* PORT: ParseVOC (USintactic.pas:324-331). */
static void parse_voc(void)
{
    do {
        scan();
        if (current_token_id == T_IDENTIFIER || current_token_id == T_NUMBER)
            parse_new_word();
        else if (current_token_id != T_SECTION_STX)
            syntax_error("Vocabulary word definition or /STX expected");
    } while (current_token_id != T_SECTION_STX);
}

/* PORT: ParseMessageList (USintactic.pas:333-354). `counter` is the
   VAR AMessageCounter - one of the frozen section counts in
   MessageList (messagelist.h's doc comment on why they are separate
   from the live table length). */
static void parse_message_list(MsgList *list, long *counter, int terminator)
{
    long value;
    const char *message;

    do {
        scan();
        if (current_token_id != terminator) {
            if (current_token_id != T_LIST_ENTRY)
                syntax_error("List entry number expected");
            if (current_int_val == MAXLONGINT)
                current_int_val = get_identifier_value();
            if (current_int_val == MAXLONGINT)
                syntax_error("Invalid or unknown symbol \"%s\"", current_text);
            value = current_int_val;
            scan();
            if (current_token_id != T_STRING)
                syntax_error("String between quotes expected");
            message = strip_quotes(current_text);
            if (value != *counter)
                syntax_error("Message/Locations/Object numbers must be consecutive");
            if (value >= MAX_MESSAGES_PER_TABLE)
                syntax_error("Message number too high. Maximum message number is %d",
                              MAX_MESSAGES_PER_TABLE - 1);
            msglist_add(list, g_arena, value, message);
            (*counter)++;
        }
    } while (current_token_id != terminator);
}

/* PORT: ParseOTX (USintactic.pas:358-365). The LAST_OBJECT/
   NUM_OBJECTS AddSymbol results are ignored, as the reference ignores
   them (a user #define colliding with them silently wins). The
   too-many-objects check is unreachable dead code (defect 19.29: the
   message-list parser above already caps entries at 255) - ported
   anyway. */
static void parse_otx(void)
{
    parse_message_list(g_messages->otx, &g_messages->otx_count, T_SECTION_LTX);
    symbols_add(g_symbols, g_arena, g_diag, "LAST_OBJECT",
                 g_messages->otx_count - 1);
    symbols_add(g_symbols, g_arena, g_diag, "NUM_OBJECTS",
                 g_messages->otx_count);
    if (g_messages->otx_count > MAX_OBJECTS)
        syntax_error("Too many objects, maximum allowed is %d", MAX_OBJECTS);
}

/* PORT: ParseLTX (USintactic.pas:367-372). */
static void parse_ltx(void)
{
    parse_message_list(g_messages->ltx, &g_messages->ltx_count, T_SECTION_CON);
    symbols_add(g_symbols, g_arena, g_diag, "LAST_LOCATION",
                 g_messages->ltx_count - 1);
    symbols_add(g_symbols, g_arena, g_diag, "NUM_LOCATIONS",
                 g_messages->ltx_count);
}

/* PORT: ParseMTX (USintactic.pas:375-378). */
static void parse_mtx(void)
{
    parse_message_list(g_messages->mtx, &g_messages->mtx_count, T_SECTION_OTX);
}

/* PORT: ParseSTX (USintactic.pas:380-383). */
static void parse_stx(void)
{
    parse_message_list(g_messages->stx, &g_messages->stx_count, T_SECTION_MTX);
}

/* PORT: ParseLocationConnections (USintactic.pas:385-408). The
   TARGET location is never validated and only an exact triple is a
   duplicate - defect 19.35, reproduced. */
static void parse_location_connections(long from_loc)
{
    VocEntry aux;
    const char *the_word;
    long direction, to_loc;

    do {
        scan();
        if (current_token_id != T_LIST_ENTRY &&
            current_token_id != T_SECTION_OBJ) {
            if (current_token_id != T_IDENTIFIER)
                syntax_error("Connection vocabulary word expected but \"%s\" found",
                              current_text);
            the_word = copy_voc(current_text);
            if (!voctree_lookup(g_voctree, g_arena, the_word, VOC_ANY, &aux))
                syntax_error("Direction is not defined:\"%s\"", current_text);
            if (aux.voc_type != VOC_VERB && aux.voc_type != VOC_NOUN)
                syntax_error("Invalid connection word");
            direction = aux.value;
            scan();
            if (current_token_id != T_IDENTIFIER &&
                current_token_id != T_NUMBER)
                syntax_error("Location number expected");
            to_loc = get_identifier_value();
            if (to_loc == MAXLONGINT)
                syntax_error("\"%s\" is not defined", current_text);
            if (connectionlist_find(g_connections, from_loc, to_loc, direction))
                syntax_error("Connection already defined");
            connectionlist_add(g_connections, g_arena, from_loc, to_loc,
                                direction);
        }
    } while (current_token_id != T_LIST_ENTRY &&
             current_token_id != T_SECTION_OBJ);
}

/* PORT: ParseCON (USintactic.pas:411-426). */
static void parse_con(void)
{
    long current_loc;

    current_loc = 0;
    scan();
    do {
        if (current_token_id != T_LIST_ENTRY)
            syntax_error("Location entry expected but \"%s\" found",
                          current_text);
        if (current_int_val == MAXLONGINT)
            current_int_val = get_identifier_value();
        if (current_int_val == MAXLONGINT)
            syntax_error("Invalid or unknown symbol \"%s\"", current_text);
        if (current_int_val != current_loc)
            syntax_error("Connections for location #%ld expected but location #%ld found",
                          current_loc, current_int_val);
        if (current_int_val >= g_messages->ltx_count)
            syntax_error("Location %ld is not defined", current_int_val);
        parse_location_connections(current_loc);
        current_loc++;
    } while (current_token_id != T_SECTION_OBJ);
    if (current_loc < g_messages->ltx_count)
        syntax_error("Connections for location #%ld missing", current_loc);
}

/* Y/N/_ helpers for ParseOBJ's three flag shapes (USintactic.pas:
   471-490): valid is IDENTIFIER folding to Y or N, or UNDERSCORE. */
static int yn_field_invalid(void)
{
    const char *up;
    if (current_token_id != T_IDENTIFIER && current_token_id != T_UNDERSCORE)
        return 1;
    if (current_token_id == T_IDENTIFIER) {
        up = str_upper_latin1(g_arena, current_text);
        if (strcmp(up, "Y") != 0 && strcmp(up, "N") != 0) return 1;
    }
    return 0;
}

static int yn_field_is_yes(void)
{
    return current_token_id == T_IDENTIFIER &&
           strcmp(str_upper_latin1(g_arena, current_text), "Y") == 0;
}

/* PORT: ParseOBJ (USintactic.pas:429-521). Custom flags are read
   FOR I := 15 DOWNTO 0 and shifted in MSB-first: the first flag
   column in the source is bit 15 (analysis 26.3). */
static void parse_obj(void)
{
    long current_obj;
    long initially_at;
    long weight;
    int container, wearable;
    unsigned flags;
    long noun, adjective;
    int i;
    VocEntry aux;
    const char *the_word;

    current_obj = 0;
    do {
        scan();
        if (current_token_id != T_SECTION_PRO) {
            if (current_token_id != T_LIST_ENTRY)
                syntax_error("Object entry expected but \"%s\" found",
                              current_text);
            if (current_int_val == MAXLONGINT)
                current_int_val = get_identifier_value();
            if (current_int_val == MAXLONGINT)
                syntax_error("Invalid or unknown symbol \"%s\"", current_text);
            if (current_int_val != current_obj)
                syntax_error("Definition for object #%ld expected but object #%ld found",
                              current_obj, current_int_val);
            if (current_int_val >= g_messages->otx_count)
                syntax_error("Object #%ld not defined", current_int_val);

            scan(); /* Get Initialy At */
            if (current_token_id != T_IDENTIFIER &&
                current_token_id != T_NUMBER &&
                current_token_id != T_UNDERSCORE)
                syntax_error("Object initial location expected but \"%s\" found",
                              current_text);
            if (current_token_id == T_UNDERSCORE)
                initially_at = LOC_NOT_CREATED;
            else {
                initially_at = get_identifier_value();
                if (initially_at == MAXLONGINT)
                    syntax_error("\"%s\" is not defined", current_text);
            }
            /* 255 (HERE) is NOT accepted; note the missing space
               before the number - the reference's own text. */
            if (initially_at >= g_messages->ltx_count &&
                initially_at != LOC_NOT_CREATED &&
                initially_at != LOC_WORN && initially_at != LOC_CARRIED)
                syntax_error("Invalid initial location%ld", initially_at);

            scan(); /* Get Weight */
            if (current_token_id != T_IDENTIFIER &&
                current_token_id != T_NUMBER)
                syntax_error("Object weight expected");
            weight = get_identifier_value();
            if (weight == MAXLONGINT)
                syntax_error("\"%s\" is not defined", current_text);
            /* No lower bound - a negative weight passes (26.3). The
               space before the colon is the reference's own. */
            if (weight > MAX_WEIGHT)
                syntax_error("Invalid weight :%s", current_text);

            scan(); /* Get if container */
            if (yn_field_invalid())
                syntax_error("\"Y\", \"N\" or \"_\" expected at container flag");
            container = yn_field_is_yes();

            scan(); /* Get if wearable */
            if (yn_field_invalid())
                syntax_error("\"Y\", \"N\" or \"_\" expected at wearable flag");
            wearable = yn_field_is_yes();

            flags = 0;
            for (i = 15; i >= 0; i--) {
                scan(); /* Get flag */
                if (yn_field_invalid())
                    syntax_error("\"Y\", \"N\" or \"_\" expected at custom flag #%d",
                                  i);
                flags = (flags << 1) & 0xFFFFu; /* Word arithmetic */
                if (yn_field_is_yes()) flags++;
            }

            scan(); /* Get Noun */
            if (current_token_id != T_IDENTIFIER &&
                current_token_id != T_NUMBER &&
                current_token_id != T_UNDERSCORE)
                syntax_error("Vocabulary noun or underscore expected but \"%s\" found",
                              current_text);
            if (current_token_id == T_UNDERSCORE) noun = NO_WORD;
            else {
                the_word = copy_voc(current_text);
                if (!voctree_lookup(g_voctree, g_arena, the_word, VOC_NOUN,
                                     &aux))
                    syntax_error("Noun not defined: \"%s\"", current_text);
                noun = aux.value;
            }

            scan(); /* Get Adject */
            if (current_token_id != T_IDENTIFIER &&
                current_token_id != T_NUMBER &&
                current_token_id != T_UNDERSCORE)
                syntax_error("Vocabulary adjective or underscore character expected but \"%s\" found",
                              current_text);
            if (current_token_id == T_UNDERSCORE) adjective = NO_WORD;
            else {
                the_word = copy_voc(current_text);
                if (!voctree_lookup(g_voctree, g_arena, the_word, VOC_ADJECT,
                                     &aux))
                    syntax_error("Adjective not defined: \"%s\"", current_text);
                adjective = aux.value;
            }
            objectlist_add(g_objects, g_arena, current_obj, noun, adjective,
                            weight, initially_at, flags, container, wearable);
            current_obj++;
        }
    } while (current_token_id != T_SECTION_PRO);
    if (current_obj < g_messages->otx_count)
        syntax_error("Definition for object #%ld missing", current_obj);
    symbols_add(g_symbols, g_arena, g_diag, "NUM_CARRIED",
                 (long)g_objects->carried_count);
    symbols_add(g_symbols, g_arena, g_diag, "NUM_WORN",
                 (long)g_objects->worn_count);
}

/* IntToStr into the arena - the token-rewrite sites (USintactic.pas:
   664, 677, 690) store the number's decimal text back into
   CurrentText. */
static const char *int_to_str(long v)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%ld", v);
    return arena_strdup(g_arena, buf);
}

/* PORT: GetWordParamValue (USintactic.pas:523-545). The Pascal takes
   a `Param: String` first argument it never reads (it uses the
   CurrentText global directly, line 528) - dropped here, PORT NOTE.
   `opcode` 255 is the untyped-retry sentinel (VOC_ANY); the six
   word-condacts force a type, SYNONYM's first parameter falling back
   from VERB to a CONVERTIBLE noun (value <= MAX_CONVERTIBLE_NAME). */
static long get_word_param_value(long opcode, int parameter_number)
{
    const char *the_word = copy_voc(current_text);
    VocType aux_type = VOC_ANY;
    VocEntry aux;
    int found;

    if (opcode != 255) {
        switch (opcode) {
        case ADJECT1_OPCODE: aux_type = VOC_ADJECT; break;
        case ADJECT2_OPCODE: aux_type = VOC_ADJECT; break;
        case ADVERB_OPCODE: aux_type = VOC_ADVERB; break;
        case NOUN2_OPCODE: aux_type = VOC_NOUN; break;
        case PREP_OPCODE: aux_type = VOC_PREPOSITION; break;
        case SYNONYM_OPCODE:
            aux_type = (parameter_number == 0) ? VOC_VERB : VOC_NOUN;
            break;
        default: break;
        }
    }
    found = voctree_lookup(g_voctree, g_arena, the_word, aux_type, &aux);
    if (!found && opcode == SYNONYM_OPCODE && parameter_number == 0) {
        /* Special case: SYNONYM may take a noun as first parameter if
           the noun is convertible. */
        found = voctree_lookup(g_voctree, g_arena, the_word, VOC_NOUN, &aux);
        if (found && aux.value > MAX_CONVERTIBLE_NAME) found = 0;
    }
    return found ? aux.value : MAXLONGINT;
}

/* Appends onto an entry's condact list, creating it on first use -
   the C shape of AddProcessCondact's VAR-parameter append onto a nil
   list (the Pascal list is nil until its first node). */
static void entry_condacts_add(Vec_ProcessCondact **list, long opcode,
                                int num_params,
                                const CondactParam params[MAX_CONDACT_PARAMS],
                                int is_db)
{
    if (*list == NULL) *list = vec_new_ProcessCondact(g_arena);
    process_condacts_add(*list, g_arena, opcode, num_params, params, is_db);
}

/* PORT: ParseProcessCondacts (USintactic.pas:547-812), whole: the
   loop skeleton, the terminator legs (another entry `>`, another
   /PRO, /END), the Unknown-condact error, the LABEL leg, the five
   directive legs (#userptr/#db/#dw/#hex/#incbin), and the
   condact-with-parameters branch (Opcode >= 0, 578-732: fake-condact
   gates, parameter resolution, indirection, inline strings,
   SKIP/PENDINGSKIP labels, semantic checks). */
static void parse_process_condacts(Vec_ProcessCondact **some_entry_condacts,
                                    long current_process, long current_entry)
{
    /* Defect 19.44: the reference leaves Opcode uninitialised, so an
       entry opening with a directive tests stack garbage. Pinned to 0
       (keep parsing) - the only outcome consistent with published
       sources; params zeroed for the same reason (28.2). */
    long opcode = 0;
    CondactParam current_condact_params[MAX_CONDACT_PARAMS] =
        {{0, 0}, {0, 0}, {0, 0}};
    long aux_long;
    int current_condact;

    current_condact = 0;
    do {
        /* Get Condact - skip the very first time, while the condact
           list is still empty, because it is already read. */
        if (*some_entry_condacts != NULL) scan();
        if (current_token_id != T_IDENTIFIER &&
            current_token_id != T_UNDERSCORE &&
            current_token_id != T_SECTION_PRO &&
            current_token_id != T_SECTION_END &&
            current_token_id != T_INCBIN &&
            current_token_id != T_DB && current_token_id != T_DW &&
            current_token_id != T_NUMBER && current_token_id != T_HEX &&
            current_token_id != T_USERPTR && current_token_id != T_LABEL &&
            current_token_id != T_PROCESS_ENTRY_SIGN)
            syntax_error("Condact, label, new process entry or new process expected but \"%s\" found",
                          current_text);

        if (current_token_id != T_INCBIN && current_token_id != T_DB &&
            current_token_id != T_DW && current_token_id != T_HEX &&
            current_token_id != T_USERPTR && current_token_id != T_LABEL) {
            if (current_token_id == T_PROCESS_ENTRY_SIGN ||
                current_token_id == T_SECTION_END ||
                current_token_id == T_SECTION_PRO) {
                opcode = -2;
            } else {
                const CondactDef *def = condact_lookup(current_text,
                                                        g_opts->v3);
                opcode = def ? def->opcode : -1; /* GetCondact */
            }

            if (opcode >= 0) {
                /* PORT: the condact branch (USintactic.pas:578-732). */
                long value;
                long max_mess = 0; /* MaXMESs - only read inside the
                                      inline-string branch that also
                                      sets it (USintactic.pas:657) */
                int num_params_at_entry;
                int i;

                /* Fake-condact gate (581-602): active ONLY with
                   -replace-xcondacts (defect 19.27 - without the
                   switch every X-condact compiles untouched). All
                   four texts carry their own trailing period - double
                   period on the wire (19.40 family). */
                if (opcode >= NUM_CONDACTS && opcode < 256) {
                    if (g_opts->replace_xcondacts) {
                        if (opcode == XPICTURE_OPCODE) {
                            if (strcmp(g_target, "CP4") != 0 &&
                                strcmp(g_target, "C64") != 0 &&
                                strcmp(g_target, "CPC") != 0 &&
                                strcmp(g_target, "MSX") != 0)
                                syntax_error("XPICTURE cannot be used in this target [%s].",
                                              g_target);
                            else
                                maluva_used = 1;
                        } else if (opcode == XSAVE_OPCODE) {
                            syntax_error("XSAVE has been deprecated, use SAVE instead.");
                        } else if (opcode == XLOAD_OPCODE) {
                            syntax_error("XLOAD has been deprecated, use LOAD instead.");
                        } else if (opcode == XBEEP_OPCODE) {
                            syntax_error("XBEEP has been deprecated, use BEEP instead.");
                        }
                    }
                }

                /* Get Parameters (605-730). The Pascal FOR evaluates
                   GetNumParams(Opcode) ONCE at loop entry - the bound
                   stays the ORIGINAL opcode's even when the inline-
                   string/PENDINGSKIP rewrites change Opcode inside
                   the loop (immaterial in the current table: every
                   rewrite pair shares a parameter count). */
                num_params_at_entry =
                    condact_by_opcode((int)opcode, g_opts->v3)->num_params;
                for (i = 0; i < num_params_at_entry; i++) {
                    int semantic_exempt;
                    scan();
                    semantic_exempt = 0;
                    current_condact_params[i].indirection = 0;
                    if (current_token_id == T_INDIRECT) {
                        /* MAX_PARAM_ACCEPTING_INDIRECTION is the
                           UConstants VAR (default 1; -v3 raises it to
                           2, drf.pas:355) - derived from v3 here per
                           the Task 6 interface note. */
                        if (i >= (g_opts->v3 ? 2 : 1))
                            syntax_error("Indirection is not allowed in this parameter");
                        current_condact_params[i].indirection = 1;
                        scan();
                    }

                    /* Inline strings and auto-numbering (617-665).
                       The TOGGLECON (520) comparison - and the MES2
                       (521) one below - can never match GetCondact's
                       0..143-plus-220 range: defect 19.28,
                       PORTED-DORMANT. */
                    if (current_token_id == T_STRING &&
                        (opcode == MESSAGE_OPCODE || opcode == MES_OPCODE ||
                         opcode == SYSMESS_OPCODE || opcode == XMES_OPCODE ||
                         opcode == XMESSAGE_OPCODE || opcode == XPLAY_OPCODE ||
                         opcode == XDATA_OPCODE ||
                         opcode == TOGGLECON_OPCODE)) {
                        semantic_exempt = 1;
                        current_text = strip_quotes(current_text);

                        /* Implements the ForceXMessages parameter. */
                        if ((opcode == MES_OPCODE || opcode == MESSAGE_OPCODE ||
                             opcode == MES2_OPCODE) &&
                            g_opts->force_x_messages) {
                            if (opcode == MES_OPCODE) opcode = XMES_OPCODE;
                            else opcode = XMESSAGE_OPCODE;
                        }

                        if ((opcode == XMES_OPCODE ||
                             opcode == XMESSAGE_OPCODE) &&
                            !g_opts->force_normal_messages) {
                            /* Source period kept - double on the wire. */
                            if (strlen(current_text) > 511)
                                syntax_error("Extended messages can be only up to 511 characters long. Your message is %lu long.",
                                              (unsigned long)strlen(current_text));
                            /* XMESSAGE gains `#n` AFTER the length
                               check - up to 513 characters stored. */
                            if (opcode == XMESSAGE_OPCODE) {
                                size_t len = strlen(current_text);
                                char *tmp = arena_alloc(g_arena, len + 3);
                                memcpy(tmp, current_text, len);
                                memcpy(tmp + len, "#n", 3);
                                current_text = tmp;
                            }
                            opcode = XMES_OPCODE;
                            current_int_val = msglist_insert_or_dedup(
                                g_messages->xtx, g_arena, current_text);
                            /* -1 is this port's stand-in for the
                               Pascal MAXLONGINT "no room" sentinel -
                               map it back so the >= MaXMESs check
                               below reads one-to-one. (XTX is
                               unlimited; dormant on this path.) */
                            if (current_int_val == -1)
                                current_int_val = MAXLONGINT;
                            max_mess = MAXLONGINT;
                        } else {
                            /* On a 16-bit machine XMESSAGEs convert
                               to normal messages (ForceNormalMessages). */
                            if (opcode == XMES_OPCODE) opcode = MES_OPCODE;
                            else if (opcode == XMESSAGE_OPCODE)
                                opcode = MESSAGE_OPCODE;

                            if (opcode == XPLAY_OPCODE ||
                                opcode == XDATA_OPCODE) {
                                current_int_val = msglist_insert_or_dedup(
                                    g_messages->other_tx, g_arena,
                                    current_text);
                                if (current_int_val == -1)
                                    current_int_val = MAXLONGINT;
                                max_mess = MAXLONGINT;
                            } else {
                                /* insertMessageFromProcess - the
                                   MTX/STX/LTX overflow cascade with
                                   the silent opcode rewrite (defect
                                   19.51; mechanics in messagelist.c,
                                   the DECISION site is here). */
                                int op_int = (int)opcode;
                                current_int_val = messagelist_insert_cascade(
                                    g_messages, g_arena, &op_int,
                                    current_text, classic_mode);
                                opcode = op_int;
                                if (current_int_val == -1)
                                    current_int_val = MAXLONGINT;
                                max_mess = MAX_MESSAGES_PER_TABLE;
                            }
                        }

                        if (current_int_val >= max_mess) {
                            if (classic_mode)
                                syntax_error("Too many messages, max messages per message table is %d",
                                              MAX_MESSAGES_PER_TABLE);
                            else
                                syntax_error("Too many messages, total messages in MTX, STX and LTX tables, plus \"MESSAGE\" strings is %d",
                                              3 * MAX_MESSAGES_PER_TABLE);
                        }
                        current_token_id = T_NUMBER;
                        value = current_int_val;
                        current_text = int_to_str(value);
                    }

                    /* SKIP with a T_LABEL parameter (668-693). */
                    if (current_token_id == T_LABEL &&
                        opcode == SKIP_OPCODE) {
                        LabelData label_data;
                        if (labels_find(g_labels, current_text,
                                         &label_data)) {
                            /* Label exists - always a BACKWARD jump;
                               only < -128 is checked. */
                            if (current_process != label_data.process)
                                syntax_error("Label \"%s\" is not in this process",
                                              current_text);
                            if (label_data.entry - current_entry - 1 < -128)
                                syntax_error("Label \"%s\" is too far from SKIP call, maximum 128 entries far allowed",
                                              current_text);
                            current_int_val =
                                label_data.entry - current_entry - 1;
                            current_text = int_to_str(current_int_val);
                            current_token_id = T_NUMBER;
                        } else {
                            /* Forward reference: placeholder label +
                               PENDINGSKIP. labels_add cannot return
                               -1 here (a real definition would have
                               been found by labels_find above); -2 is
                               the table-full guard, already
                               diagnosed. */
                            long label_id = labels_add(g_labels, g_arena,
                                                        g_diag, current_text,
                                                        -1, -1, 1, -1);
                            if (label_id == -2) stop_already_diagnosed();
                            diag_verbose(g_diag,
                                          "Forward declaration of label %s created.",
                                          current_text);
                            opcode = PENDINGSKIP_OPCODE;
                            current_condact_params[0].indirection = 0;
                            current_condact_params[0].value = label_id;
                            current_token_id = T_NUMBER;
                            current_text = int_to_str(label_id);
                            current_int_val = label_id;
                        }
                    }

                    if (current_token_id != T_NUMBER &&
                        current_token_id != T_IDENTIFIER &&
                        current_token_id != T_UNDERSCORE &&
                        current_token_id != T_STRING)
                        syntax_error("Invalid condact parameter");

                    /* Value resolution precedence (697-710, analysis
                       24.4): string expression, underscore, typed
                       vocabulary for the six word-condacts, symbol
                       table / numeric literal, untyped vocabulary. */
                    value = MAXLONGINT;
                    if (current_token_id == T_STRING)
                        value = get_expression_value();
                    if (current_token_id == T_UNDERSCORE) value = NO_WORD;
                    if (value == MAXLONGINT &&
                        (opcode == SYNONYM_OPCODE || opcode == PREP_OPCODE ||
                         opcode == NOUN2_OPCODE || opcode == ADJECT1_OPCODE ||
                         opcode == ADVERB_OPCODE || opcode == ADJECT2_OPCODE))
                        value = get_word_param_value(opcode, i);
                    if (value == MAXLONGINT) value = get_identifier_value();
                    if (value == MAXLONGINT)
                        value = get_word_param_value(255, 0);
                    if (value == MAXLONGINT)
                        syntax_error("Invalid parameter #%d: \"%s\" for condact %s",
                                      i + 1, current_text,
                                      condact_by_opcode((int)opcode,
                                                         g_opts->v3)->name);
                    /* SKIP negative wrap - the ONLY floor check a
                       literal numeric SKIP gets (defect 19.37). */
                    if (opcode == SKIP_OPCODE && value < 0)
                        value = 256 + value;
                    if (opcode == XMES_OPCODE || opcode == XMESSAGE_OPCODE) {
                        /* XTX indices may exceed 255. */
                        if (value < 0)
                            syntax_error("Invalid parameter value \"%s\" for condact %s",
                                          current_text,
                                          condact_by_opcode((int)opcode,
                                                             g_opts->v3)->name);
                    } else if (value < 0 || value > MAX_PARAMETER_RANGE) {
                        syntax_error("Invalid parameter value \"%s\" for condact %s",
                                      current_text,
                                      condact_by_opcode((int)opcode,
                                                         g_opts->v3)->name);
                    }

                    current_condact_params[i].value = value;

                    /* Semantic check (720-728, analysis 25): skipped
                       for an indirected or inline-string-exempt
                       parameter and under -no-semantic;
                       -semantic-warnings downgrades to Warning. */
                    if (!current_condact_params[i].indirection &&
                        !semantic_exempt && !g_opts->no_semantic) {
                        const char *sem;
                        /* Abort-parity guard: SemanticCheck's
                           ParamValue is a Pascal Byte (UCondacts.pas:
                           246); USintactic.pas:722 narrows Longint to
                           Byte under {$R+}, so XMES/XMESSAGE with a
                           numeric param > 255 dies with an unhandled
                           ERangeError, exit 217 (live-verified: `XMES
                           300`), identically under -semantic-warnings;
                           -no-semantic skips the call and the raw
                           value flows on untruncated. FATAL with a
                           clear text instead - the FPC runtime
                           message is unreproducible. */
                        if (value > 255)
                            fatal_jmp("parameter value %ld for condact %s "
                                      "is outside the semantic check's byte "
                                      "range (the reference crashes with an "
                                      "unhandled ERangeError here; "
                                      "-no-semantic compiles it)",
                                      value,
                                      condact_by_opcode((int)opcode,
                                                         g_opts->v3)->name);
                        sem = condact_semantic_check(
                            g_arena, (int)opcode, g_opts->v3, i + 1,
                            (int)value, current_text, g_voctree,
                            g_messages);
                        if (sem != NULL && sem[0] != '\0') {
                            if (g_opts->semantic_warnings)
                                warning("%s", sem);
                            else
                                syntax_error("%s", sem);
                        }
                    }
                }
                entry_condacts_add(some_entry_condacts, opcode,
                                    condact_by_opcode((int)opcode,
                                                       g_opts->v3)->num_params,
                                    current_condact_params, 0);
            } else {
                if (opcode == -1)
                    syntax_error("Unknown condact: \"%s\"", current_text);
            }
        } else if (current_token_id == T_LABEL) { /* LABEL */
            long r = labels_add(g_labels, g_arena, g_diag, current_text,
                                 current_process, current_entry + 1, 0, -1);
            if (r == -1)
                syntax_error("Label already defined (%s) or too many labels",
                              current_text);
            if (r == -2) stop_already_diagnosed(); /* labels.h's guard */
            diag_verbose(g_diag, "Label %s created at process #%ld, entry #%ld.",
                          current_text, current_process, current_entry + 1);
            scan();
            opcode = -1;
            current_condact = current_condact - 1;
        } else if (current_token_id == T_USERPTR) { /* USERPTR */
            scan();
            if (current_token_id != T_NUMBER)
                syntax_error("#userptr parameter should be numeric");
            if (current_int_val < 0 || current_int_val > 9)
                syntax_error("#userptr parameter should be 0-9");
            diag_verbose(g_diag, "#USERPTR %s processed", current_text);
            current_condact_params[0].value = current_int_val;
            current_condact_params[0].indirection = 0;
            entry_condacts_add(some_entry_condacts, FAKE_USERPTR_CONDACT_CODE,
                                1, current_condact_params, 0);
        } else if (current_token_id == T_DB) { /* DB */
            scan();
            aux_long = extract_value(NULL);
            /* PORTED-DORMANT: unreachable - extract_value halts
               internally on an unknown value first (defect 19.22's
               dead guard). */
            if (aux_long == MAXLONGINT)
                syntax_error("#DB Unknown value \"%s\"", current_text);
            if (aux_long < 0 || aux_long > 255)
                syntax_error("DB value should be between 0 and 255");
            diag_verbose(g_diag, "#DB %s(%ld) processed", current_text,
                          aux_long);
            /* Defect 19.22, REPRODUCED: the queued byte is
               CurrentIntVal - still the lexer's MAXLONGINT filler for
               a symbol operand - NOT the range-checked aux_long
               (live-verified: `#db SYM` emits Opcode 2147483647). */
            entry_condacts_add(some_entry_condacts, current_int_val, 0,
                                current_condact_params, 1);
        } else if (current_token_id == T_DW) { /* DW */
            scan();
            aux_long = extract_value(NULL);
            /* PORTED-DORMANT: unreachable, as #DB's twin above. */
            if (aux_long == MAXLONGINT)
                syntax_error("#DW Unknown value \"%s\"", current_text);
            if (aux_long < 0 || aux_long > 65535)
                syntax_error("DW value should be between 0 and 65535");
            diag_verbose(g_diag, "#DW %s(%ld) processed", current_text,
                          aux_long);
            /* Defect 19.22 again: two bytes from CurrentIntVal,
               low then high (a symbol operand queues 0xFF,0xFF). */
            entry_condacts_add(some_entry_condacts, current_int_val & 0xFF, 0,
                                current_condact_params, 1);
            entry_condacts_add(some_entry_condacts,
                                (current_int_val & 0xFF00) >> 8, 0,
                                current_condact_params, 1);
        } else if (current_token_id == T_HEX) { /* HEX */
            const char *hex_string;
            scan();
            if (current_token_id != T_STRING)
                syntax_error("HEX parameter should in between quotes");
            /* The parity check runs on the FULL text, delimiters
               included (USintactic.pas:782 checks CurrentText before
               stripping). */
            if (strlen(current_text) % 2 != 0)
                syntax_error("Invalid hexadecimal string");
            hex_string = strip_quotes(current_text);
            while (*hex_string) {
                int hi, lo, k;
                int dig[2];
                for (k = 0; k < 2; k++) {
                    int c = (unsigned char)hex_string[k];
                    if (c >= '0' && c <= '9') dig[k] = c - '0';
                    else if (c >= 'A' && c <= 'F') dig[k] = c - 'A' + 10;
                    else if (c >= 'a' && c <= 'f') dig[k] = c - 'a' + 10;
                    else {
                        /* PORT NOTE (abort-parity deviation, defect
                           19.34): the reference's Hex2Dec raises an
                           uncaught EConvertError - an FPC runtime
                           abort whose text is unreproducible - so
                           this guard FATALs with a clear diagnostic
                           instead, same precedent as lex.c's
                           out-of-int32 guard. */
                        fatal_jmp("invalid hexadecimal digit '%c' in #hex "
                                  "string (the reference crashes with an "
                                  "unhandled EConvertError here)",
                                  hex_string[k]);
                    }
                }
                hi = dig[0]; lo = dig[1];
                entry_condacts_add(some_entry_condacts, hi * 16 + lo, 0,
                                    current_condact_params, 1);
                hex_string += 2;
            }
            diag_verbose(g_diag, "#HEX %s processed", current_text);
        } else if (current_token_id == T_INCBIN) { /* INCBIN */
            const char *filename;
            FILE *f;
            int c;
            scan();
            if (current_token_id != T_STRING)
                syntax_error("Included file should be in between quotes");
            filename = strip_quotes(current_text);
            if (!file_exists(filename))
                syntax_error("Included file \"%s\" not found", filename);
            f = fopen(filename, "rb");
            if (f == NULL) {
                /* Guard only: exists-check just passed (FPC's Reset
                   would raise a runtime error on this race). */
                fatal_jmp("cannot open \"%s\"", filename);
            }
            diag_verbose(g_diag, "#incbin \"%s\" processed.", filename);
            while ((c = fgetc(f)) != EOF)
                entry_condacts_add(some_entry_condacts, c, 0,
                                    current_condact_params, 1);
            fclose(f);
        }
        current_condact = current_condact + 1;
    } while (opcode >= 0);
    /* current_condact is write-only in the reference too (defect
       19.30's dead-code cluster) - kept for one-to-one flow. */
    (void)current_condact;
}

/* PORT: ParseVerbNoun (USintactic.pas:814-846). A verb slot accepts a
   VERB or a convertible NOUN (value <= MAX_CONVERTIBLE_NAME). */
static void parse_verb_noun(long *verb, long *noun)
{
    const char *the_word;
    VocEntry aux;
    int valid_verb;
    int found;

    scan(); /* Get the verb */
    if (current_token_id != T_NUMBER && current_token_id != T_IDENTIFIER &&
        current_token_id != T_UNDERSCORE)
        syntax_error("Vocabulary verb expected but \"%s\" found",
                      current_text);
    if (current_token_id == T_UNDERSCORE) *verb = NO_WORD;
    else {
        the_word = copy_voc(current_text);
        valid_verb = 0;
        found = voctree_lookup(g_voctree, g_arena, the_word, VOC_ANY, &aux);
        if (found) {
            if (aux.voc_type == VOC_VERB) valid_verb = 1;
            else if (aux.voc_type == VOC_NOUN &&
                     aux.value <= MAX_CONVERTIBLE_NAME)
                valid_verb = 1;
        }
        if (!valid_verb)
            syntax_error("Verb not found in vocabulary: \"%s\"",
                          current_text);
        *verb = aux.value;
    }

    scan(); /* Get Noun */
    if (current_token_id != T_IDENTIFIER && current_token_id != T_NUMBER &&
        current_token_id != T_UNDERSCORE)
        syntax_error("Vocabulary noun expected but \"%s\" found",
                      current_text);
    if (current_token_id == T_UNDERSCORE) *noun = NO_WORD;
    else {
        the_word = copy_voc(current_text);
        if (!voctree_lookup(g_voctree, g_arena, the_word, VOC_NOUN, &aux))
            syntax_error("Noun not found in vocabulary: \"%s\"",
                          current_text);
        *noun = aux.value;
    }
}

/* PORT: ParseProcessEntries (USintactic.pas:876-915). Synonym `>`
   headers accumulate verb/noun pairs and every copy is added with the
   SAME condact-list pointer (shared structure, the substrate of
   defects 19.42/19.24 - Task 7 tests those against the label legs).
   Note the two spaces in `expected  but` - the reference's own. */
static void parse_process_entries(long current_process)
{
    long verb, noun;
    Vec_ProcessCondact *entry_condacts;
    long *verb_nouns = NULL;
    size_t vn_len = 0, vn_cap = 0;
    size_t i;
    long current_entry;

    current_entry = 0;
    scan(); /* Get > sign, label, or next process */
    do {
        if (current_token_id != T_PROCESS_ENTRY_SIGN &&
            current_token_id != T_SECTION_PRO &&
            current_token_id != T_SECTION_END &&
            current_token_id != T_LABEL)
            syntax_error("Label or entry sign \">\" expected  but \"%s\" found",
                          current_text);

        if (current_token_id != T_SECTION_PRO &&
            current_token_id != T_SECTION_END) {
            if (current_token_id == T_LABEL) { /* A label */
                long r = labels_add(g_labels, g_arena, g_diag, current_text,
                                     current_process, current_entry, 0, -1);
                if (r == -1)
                    syntax_error("Label already defined (%s) or too many labels",
                                  current_text);
                if (r == -2) stop_already_diagnosed();
                diag_verbose(g_diag,
                              "Label %s created at process #%ld, entry #%ld.",
                              current_text, current_process, current_entry);
                scan();
            } else { /* An entry */
                vn_len = 0;
                do { /* per synonym entry */
                    parse_verb_noun(&verb, &noun);
                    if (vn_len + 2 > vn_cap) {
                        size_t new_cap = vn_cap ? vn_cap * 2 : 8;
                        long *grown = arena_alloc(g_arena,
                                                   sizeof(long) * new_cap);
                        if (vn_len)
                            memcpy(grown, verb_nouns, sizeof(long) * vn_len);
                        verb_nouns = grown;
                        vn_cap = new_cap;
                    }
                    verb_nouns[vn_len++] = verb;
                    verb_nouns[vn_len++] = noun;
                    scan();
                } while (current_token_id == T_PROCESS_ENTRY_SIGN);
                entry_condacts = NULL;
                parse_process_condacts(&entry_condacts, current_process,
                                        current_entry);
                /* Dump condacts once per synonym entry - the SAME
                   list pointer each time. */
                for (i = 0; i < vn_len / 2; i++) {
                    ProcessSlot *slot = processtable_at(g_processes, g_arena,
                                                         g_diag,
                                                         current_process);
                    if (slot == NULL) stop_already_diagnosed();
                    process_add_entry(slot, g_arena, verb_nouns[i * 2],
                                       verb_nouns[i * 2 + 1], NULL,
                                       entry_condacts);
                }
                current_entry = current_entry + (long)(vn_len / 2);
            }
        }
    } while (current_token_id != T_SECTION_PRO &&
             current_token_id != T_SECTION_END);
}

/* PORT: ParsePRO (USintactic.pas:918-932). No upper bound on the
   process number in the reference (defect 19.36 - FPC dies with a
   range-check RTE); processtable_at carries the PORT-NOTEd guard. */
static void parse_pro(void)
{
    long proc_num;

    g_processes = processtable_new(g_arena); /* InitializeProcesses() */
    g_last_process = -1;
    do {
        scan();
        if (current_token_id != T_IDENTIFIER &&
            current_token_id != T_NUMBER)
            syntax_error("Process number expected but \"%s\" found",
                          current_text);
        proc_num = get_identifier_value();
        if (proc_num == MAXLONGINT)
            syntax_error("\"%s\" is not defined", current_text);
        if (proc_num > g_last_process) g_last_process = proc_num;
        {
            ProcessSlot *slot = processtable_at(g_processes, g_arena, g_diag,
                                                 proc_num);
            if (slot == NULL) stop_already_diagnosed(); /* 19.36 guard */
            if (vec_len_ProcessEntry(slot->entries) != 0)
                warning("Process #%ld already defined, concatenating entries",
                         proc_num);
        }
        parse_process_entries(proc_num);
    } while (current_token_id != T_SECTION_END);
}

/* ---- public surface ---- */

void sintactic_init(Arena *a, Diag *d)
{
    g_arena = a;
    g_diag = d;
    g_symbols = symbols_new(a);
    g_voctree = voctree_new(a);
    g_messages = messagelist_new(a);
    g_connections = connectionlist_new(a);
    g_objects = objectlist_new(a);
    g_processes = processtable_new(a);
    g_last_process = -1;
    g_labels = labels_new(a);
    g_externs = ctlextern_new(a);
    classic_mode = 0;
    debug_mode = 0;
    maluva_used = 0;
    g_target = "";
    g_subtarget = "";
    global_nested_ifdef_count = 0;
    cur = NULL;
    current_text = "";
    current_int_val = 0;
    current_token_id = T_NOTHING;
    curr_lineno = 0;
    curr_colno = 0;
}

int sintactic_parse(Arena *a, Diag *d, Token *stream,
                     const FrontOptions *opts)
{
    /* PORT NOTE: drf.pas:230 prepends a fake T_NOTHING token to the
       list before lexing ("it will be never loaded... first time this
       fake one will be skipped") so the first Scan() lands on the
       first real token. lex_tokenize returns the REAL head, so the
       fake head is built here instead - same effect, moved to the one
       consumer that needs it. */
    Token fake;

    if (g_symbols == NULL || g_arena != a || g_diag != d) {
        fprintf(stderr, "ndrc: internal error, sintactic_parse called "
                        "without a matching sintactic_init\n");
        abort();
    }
    g_opts = opts;

    fake.id = T_NOTHING;
    fake.text = "";
    fake.value = TOKEN_NO_VALUE;
    fake.line = 0;
    fake.col = 0;
    fake.next = stream;

    /* PORT: Sintactic's entry resets (USintactic.pas:936-947).
       XTXCount is NOT among the reset counters (defect 19.38, dead in
       a single-shot process - messagelist_new zeroed it anyway). The
       remaining containers (symbols, vocabulary, connections, message
       LISTS, labels) are not reset here either, exactly as the
       reference does not - sintactic_init owns fresh instances. */
    cur = &fake;
    classic_mode = 0;
    maluva_used = 0;
    debug_mode = 0;
    g_messages->mtx_count = 0;
    g_messages->stx_count = 0;
    g_messages->ltx_count = 0;
    g_messages->otx_count = 0;
    g_messages->other_tx_count = 0;
    g_target = opts->target ? opts->target : "";
    g_subtarget = opts->subtarget ? opts->subtarget : "";
    global_nested_ifdef_count = 0;
    curr_lineno = 0;
    curr_colno = 0;

    if (setjmp(g_jmp) != 0) return diag_exit_code(d);

    parse_ctl();
    parse_voc();
    parse_stx();
    parse_mtx();
    parse_otx();
    parse_ltx();
    parse_con();
    parse_obj();
    parse_pro();
    if (global_nested_ifdef_count != 0)
        syntax_error("%u #endif(s) missing.", global_nested_ifdef_count);
    return 0;
}

/* PORT: FixForwardLabels (USintactic.pas:56-93). Walks every
   process's entries; in each entry's condact list the FIRST
   PENDINGSKIP found is resolved against the label table and rewritten
   to a real SKIP with displacement Entry - EntryNo - 1, then the scan
   BREAKs - a second forward label in the same entry is never resolved
   and its placeholder opcode leaks into the output (defect 19.43).
   Because synonym copies share ONE condact list, the shared node is
   fixed once with the FIRST copy's entry number (defect 19.42).
   Errors report at whatever position /END left in the globals. */
static void fix_forward_labels(void)
{
    long procno;
    size_t entryno, n_entries, c, n_condacts;

    for (procno = 0; procno <= g_last_process; procno++) {
        const ProcessSlot *slot = processtable_get(g_processes, procno);
        if (slot == NULL) continue; /* > 255 cannot survive parse_pro */
        n_entries = vec_len_ProcessEntry(slot->entries);
        for (entryno = 0; entryno < n_entries; entryno++) {
            Vec_ProcessCondact *condacts =
                vec_at_ProcessEntry(slot->entries, entryno)->condacts;
            n_condacts = condacts ? vec_len_ProcessCondact(condacts) : 0;
            for (c = 0; c < n_condacts; c++) {
                ProcessCondact *pc = vec_at_ProcessCondact(condacts, c);
                const LabelData *ld;
                if (pc->opcode != PENDINGSKIP_OPCODE) continue;
                /* The reference indexes LabelList[Params[0].Value]
                   unconditionally; a #db byte of 141 matches too (no
                   is_db exclusion). Out-of-range reads a zeroed
                   Pascal global slot - reproduced via a zeroed
                   record, not an OOB read. */
                ld = labels_at(g_labels, (size_t)pc->params[0].value);
                if (ld == NULL) {
                    static const LabelData zeroed = {"", 0, 0, 0, 0};
                    ld = &zeroed;
                }
                if (ld->is_forward)
                    syntax_error("Label %s was referenced but then not defined",
                                  ld->skip_label);
                if (ld->process != procno)
                    syntax_error("Label %s was referenced in one process but defined in a different process",
                                  ld->skip_label);
                if (ld->entry - (long)entryno > 128)
                    syntax_error("SKIP using label %s trys to jump forward too much, maximum 128 entries jumped allowed",
                                  ld->skip_label);
                diag_verbose(g_diag,
                              "Forward reference of label \"%s\" found at process #%ld, entry #%ld.",
                              ld->skip_label, ld->process, ld->entry);
                pc->opcode = SKIP_OPCODE;
                pc->params[0].value = ld->entry - (long)entryno - 1;
                break; /* only the FIRST per entry - defect 19.43 */
            }
        }
    }
}

int sintactic_fix_forward_labels(void)
{
    /* Fresh jump target: the one from sintactic_parse died with its
       stack frame. Same error model - SyntaxError longjmps out, the
       exit class is diag's. */
    if (setjmp(g_jmp) != 0) return diag_exit_code(g_diag);
    fix_forward_labels();
    return 0;
}

SymbolList *sintactic_symbols(void) { return g_symbols; }
VocTree *sintactic_voctree(void) { return g_voctree; }
MessageList *sintactic_messages(void) { return g_messages; }
ConnectionList *sintactic_connections(void) { return g_connections; }
ObjectList *sintactic_objects(void) { return g_objects; }
ProcessTable *sintactic_processes(void) { return g_processes; }
LabelTable *sintactic_labels(void) { return g_labels; }
CTLExternList *sintactic_externs(void) { return g_externs; }

int sintactic_classic_mode(void) { return classic_mode; }
int sintactic_debug_mode(void) { return debug_mode; }
int sintactic_maluva_used(void) { return maluva_used; }
long sintactic_last_process(void) { return g_last_process; }
