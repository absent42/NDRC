/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/jsonexport.c - Copyright (C) 2026 Dan Gibson.

   PORT: UJSONExport.pas (D:/DRC/src, branch nextdaad), read WHOLE:
   FixDoubleQuotes (UJSONExport.pas:38-43) escapes `"` but never a bare
   backslash, so a backslash byte reaching the output produces invalid
   JSON - REPRODUCED, not fixed; ConvertChars' (UJSONExport.pas:157-265)
   own inline comments are unreliable - the tables below transcribe the
   ACTUAL executed branches, never the comments.

   ARCHITECTURE: the whole file is built into one in-memory Str, then
   written in a single fwrite to a file opened "wb" (binary mode) -
   the ONLY way to get CRLF UNCONDITIONALLY on every platform in C
   (main.c/layout.c already establish this "wb" + one fwrite idiom for
   every other NDRC output file; text mode would let the CRT's own
   newline translation double up the CRLF this file writes explicitly,
   or drop it entirely on a non-Windows CRT - PORT NOTE, pins the
   reference platform's FPC WriteLn(JSON,...) behaviour per
   constraints.md). Every "\r\n" below is one WriteLn; the bare "\n"
   inside the symbols/vocabulary builders is the literal #10 the
   Pascal bakes into those two strings BEFORE they ever reach a
   WriteLn (idiom A) - never a "\r\n" there. */
#include "jsonexport.h"

#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "condacts.h"
#include "connections.h"
#include "constants.h"
#include "ctlextern.h"
#include "messagelist.h"
#include "objects.h"
#include "process.h"
#include "str.h"
#include "symbols.h"
#include "voctree.h"

/* ---- tabs() (UJSONExport.pas:15,17-24) ---- */

static void emit_tabs(Str *out, int n)
{
    while (n-- > 0) str_push(out, '\t');
}

/* ---- AnsiReplaceStr, as ConvertChars/ConvertAscii7Chars actually use
   it: every occurrence of `needle` replaced by `repl`, scanning
   left-to-right, non-overlapping (Delphi/FPC's default rfReplaceAll
   semantics - no rfIgnoreCase anywhere in the reference's calls, so
   this is byte-exact case-sensitive). `hay`/`hay_len` need not be
   NUL-terminated (n is authoritative), matching str.h's own binary-
   safe convention. */

static Str *replace_all(Arena *a, const char *hay, size_t hay_len,
                         const char *needle, const char *repl)
{
    Str *out = str_new(a);
    size_t nlen = strlen(needle);
    size_t i = 0;

    if (nlen == 0) {
        str_append_n(out, hay, hay_len);
        return out;
    }
    while (i < hay_len) {
        if (i + nlen <= hay_len && memcmp(hay + i, needle, nlen) == 0) {
            str_append(out, repl);
            i += nlen;
        } else {
            str_push(out, hay[i]);
            i++;
        }
    }
    return out;
}

static Str *replace_all_str(Arena *a, Str *in, const char *needle,
                             const char *repl)
{
    return replace_all(a, str_cstr(in), str_len(in), needle, repl);
}

/* ---- FixDoubleQuotes (UJSONExport.pas:38-43) ---- */

static Str *fix_double_quotes(Arena *a, const char *text)
{
    Str *step1 = replace_all(a, text, strlen(text), "\"", "\\\"");
    /* needle: backslash,backslash,quote (3 bytes, Pascal '\\"');
       replacement: backslash,quote (2 bytes, Pascal '\"'). */
    return replace_all_str(a, step1, "\\\\\"", "\\\"");
}

/* ---- ConvertChars / ConvertAscii7Chars per-byte case tables ----

   Transcribed directly from UJSONExport.pas:168-241 (ConvertChars) and
   56-129 (ConvertAscii7Chars) - the CASE(Ord(Str[i])) OF blocks, never
   from the source's own (unreliable, 19.5) inline comments. */

typedef struct { unsigned char byte; const char *rep; } ByteMap;

/* ConvertChars: direct unicode-escape substitutions, no wrapper
   (UJSONExport.pas:170-185,229 - the 223/sharp-s row sits under the
   "other chars" comment but emits a plain \u007F, not a #g/#t wrap). */
static const ByteMap CC_DIRECT[] = {
    {170, "\\u0010"}, {161, "\\u0011"}, {191, "\\u0012"}, {171, "\\u0013"},
    {187, "\\u0014"}, {225, "\\u0015"}, {233, "\\u0016"}, {237, "\\u0017"},
    {243, "\\u0018"}, {250, "\\u0019"}, {241, "\\u001A"}, {209, "\\u001B"},
    {231, "\\u001C"}, {199, "\\u001D"}, {252, "\\u001E"}, {220, "\\u001F"},
    {223, "\\u007F"},
};

/* ConvertChars: #g<code>#t-wrapped substitutions - `rep` is the raw
   4-hex-digit code text placed between the wrapper markers
   (UJSONExport.pas:187-225,231-241). */
static const ByteMap CC_WRAPPED[] = {
    {193, "\\u007B"}, {201, "\\u007C"}, {205, "\\u007D"}, {211, "\\u007E"},
    {218, "\\u007F"},
    {224, "\\u0010"}, {227, "\\u0011"}, {228, "\\u0012"}, {226, "\\u0013"},
    {232, "\\u0014"}, {235, "\\u0015"}, {234, "\\u0016"}, {236, "\\u0017"},
    {239, "\\u0018"}, {238, "\\u0019"}, {242, "\\u001A"}, {245, "\\u001B"},
    {246, "\\u001C"}, {244, "\\u001D"}, {249, "\\u001E"}, {251, "\\u001F"},
    {192, "\\u0020"}, {195, "\\u0021"}, {196, "\\u0022"}, {194, "\\u0023"},
    {200, "\\u0024"}, {203, "\\u0025"}, {202, "\\u0026"}, {204, "\\u0027"},
    {207, "\\u0028"}, {206, "\\u0029"}, {210, "\\u002A"}, {213, "\\u002B"},
    {214, "\\u002C"}, {212, "\\u002D"}, {217, "\\u002E"}, {219, "\\u002F"},
    {253, "\\u003A"}, {221, "\\u003B"}, {254, "\\u003C"}, {222, "\\u003D"},
    {229, "\\u003E"}, {197, "\\u003F"},
    {240, "\\u005B"}, {208, "\\u005C"}, {248, "\\u005D"}, {216, "\\u005E"},
};

/* ConvertAscii7Chars: every byte folds to a plain ASCII letter/digraph,
   never a unicode escape (UJSONExport.pas:58-129) - same source-byte
   set as CC_DIRECT+CC_WRAPPED combined (64 bytes each side), different
   target text throughout. */
static const ByteMap A7_DIRECT[] = {
    {170, "a"}, {161, "#"}, {191, "#"}, {171, "<<"}, {187, ">>"},
    {225, "a"}, {233, "e"}, {237, "i"}, {243, "o"}, {250, "u"},
    {241, "ny"}, {209, "NY"}, {231, "c"}, {199, "C"}, {252, "u"}, {220, "U"},
    {193, "A"}, {201, "E"}, {205, "I"}, {211, "O"}, {218, "U"},
    {224, "a"}, {227, "a"}, {228, "a"}, {226, "a"},
    {232, "e"}, {235, "e"}, {234, "e"},
    {236, "i"}, {239, "i"}, {238, "i"},
    {242, "o"}, {245, "o"}, {246, "o"}, {244, "o"},
    {249, "u"}, {251, "u"},
    {192, "A"}, {195, "A"}, {196, "A"}, {194, "A"},
    {200, "E"}, {203, "E"}, {202, "E"},
    {204, "I"}, {207, "I"}, {206, "I"},
    {210, "O"}, {213, "O"}, {214, "O"}, {212, "O"},
    {217, "U"}, {219, "U"},
    {223, "ss"},
    {253, "y"}, {221, "Y"}, {254, "th"}, {222, "Th"}, {229, "a"}, {197, "A"},
    {240, "d"}, {208, "D"}, {248, "o"}, {216, "O"},
};

static const char *map_lookup(const ByteMap *map, size_t n, unsigned char byte)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (map[i].byte == byte) return map[i].rep;
    return NULL;
}

#define MAP_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The per-character CASE block: `wrapped`/`nwrapped` may be NULL/0
   (ConvertAscii7Chars has no wrapper concept at all). */
static Str *apply_case_map(Arena *a, const char *text,
                            const ByteMap *direct, size_t ndirect,
                            const ByteMap *wrapped, size_t nwrapped)
{
    Str *out = str_new(a);
    size_t i, n = strlen(text);

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        const char *rep = map_lookup(direct, ndirect, c);
        if (rep) { str_append(out, rep); continue; }
        if (nwrapped) {
            rep = map_lookup(wrapped, nwrapped, c);
            if (rep) {
                str_append(out, "#g");
                str_append(out, rep);
                str_append(out, "#t");
                continue;
            }
        }
        str_push(out, (char)c);
    }
    return out;
}

/* The escape-sequence pass run over the WHOLE string after the
   per-character block, in this exact order (UJSONExport.pas:248-263 /
   136-151): #e, #g, #t, #b, #s, #f, #k, #n, #r, literal backslash-n,
   literal backslash-r, then #A..#P. `e_repl`/`f_repl` are the two
   values that differ between ConvertChars and ConvertAscii7Chars. */
static Str *apply_escape_pass(Arena *a, Str *in, const char *e_repl,
                               const char *f_repl)
{
    Str *cur = in;
    int k;

    cur = replace_all_str(a, cur, "#e", e_repl);
    cur = replace_all_str(a, cur, "#g", "\\u000e");
    cur = replace_all_str(a, cur, "#t", "\\u000f");
    cur = replace_all_str(a, cur, "#b", "\\u000b");
    cur = replace_all_str(a, cur, "#s", " ");
    cur = replace_all_str(a, cur, "#f", f_repl);
    cur = replace_all_str(a, cur, "#k", "\\u000c");
    cur = replace_all_str(a, cur, "#n", "\\u000d");
    cur = replace_all_str(a, cur, "#r", "\\u000d");
    cur = replace_all_str(a, cur, "\\n", "\\u000d"); /* literal \,n */
    cur = replace_all_str(a, cur, "\\r", "\\u000d"); /* literal \,r */
    for (k = 0; k < 16; k++) {
        char needle[3];
        char repl[8];
        needle[0] = '#';
        needle[1] = (char)('A' + k);
        needle[2] = '\0';
        snprintf(repl, sizeof repl, "\\u00%02X", 0x10 + k);
        cur = replace_all_str(a, cur, needle, repl);
    }
    return cur;
}

/* PORT: ConvertChars (UJSONExport.pas:157-266). */
static Str *convert_chars(Arena *a, const char *text)
{
    Str *cased = apply_case_map(a, text, CC_DIRECT, MAP_LEN(CC_DIRECT),
                                 CC_WRAPPED, MAP_LEN(CC_WRAPPED));
    return apply_escape_pass(a, cased, "#g\\u0060#t", "\\u007f");
}

/* PORT: ConvertAscii7Chars (UJSONExport.pas:45-154). */
static Str *convert_ascii7_chars(Arena *a, const char *text)
{
    Str *cased = apply_case_map(a, text, A7_DIRECT, MAP_LEN(A7_DIRECT),
                                 NULL, 0);
    return apply_escape_pass(a, cased, "e", "");
}

/* Text field value: FixDoubleQuotes THEN Convert{Ascii7,}Chars, per
   the call sites at UJSONExport.pas:399-400 (`ConvertChars(
   FixDoubleQuotes(...))`, quoting applied first). */
static Str *convert_text(Arena *a, const char *text, int ascii7)
{
    Str *quoted = fix_double_quotes(a, text);
    return ascii7 ? convert_ascii7_chars(a, str_cstr(quoted))
                   : convert_chars(a, str_cstr(quoted));
}

/* ---- settings (UJSONExport.pas:286-293) ----

   The ONE object written on a single physical line, comma-space
   separated fields - every other object in this file is multi-line.
   maluva_used = MaluvaUsed OR NOT CheckMaluva. */
static void emit_settings(Str *out, int indent, const FrontOptions *opts)
{
    int classic = sintactic_classic_mode() ? 1 : 0;
    int debug = sintactic_debug_mode() ? 1 : 0;
    int v3code = opts->v3 ? 1 : 0;
    int maluva_used = (sintactic_maluva_used() || !opts->check_maluva) ? 1 : 0;

    emit_tabs(out, indent);
    str_append(out, "\"settings\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");
    emit_tabs(out, indent);
    str_appendf(out,
                "{\"classic_mode\":%d, \"debug_mode\":%d, \"v3code\":%d, "
                "\"maluva_used\":%d}\r\n",
                classic, debug, v3code, maluva_used);
    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- symbols (UJSONExport.pas:26-30,295-302) - idiom A, REVERSE
   insertion order (getSymbolsJSON, UJSONExport.pas:29, concatenates
   the recursive call on ^.Next BEFORE appending its own line).
   symbols_at is insertion-ordered (index 0 = oldest); walking index
   count-1 downto 0 reproduces that Next-first recursion (the tail -
   most recently defined - emitted first). */
static void emit_symbols(Arena *a, Str *out, int indent)
{
    SymbolList *syms = sintactic_symbols();
    size_t n = symbols_count(syms);
    Str *body = str_new(a);
    size_t i;

    emit_tabs(out, indent);
    str_append(out, "\"symbols\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");

    for (i = n; i-- > 0;) {
        const char *name;
        long value;
        symbols_at(syms, i, &name, &value);
        emit_tabs(body, indent);
        emit_tabs(body, indent);
        str_appendf(body, "{\"symbol\":\"%s\", \"Value\":%ld},\n", name, value);
    }
    /* SetLength's trim underflows on an empty list; the reference does
       NOT abort - it emits a blank line then the WriteLn CRLF.
       Unreachable for symbols, reachable for vocabulary - see
       emit_vocabulary. Live-pinned 2026-08-27. */
    if (str_len(body) >= 2)
        str_append_n(out, str_bytes(body), str_len(body) - 2);
    str_append(out, "\r\n");

    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- externs (UJSONExport.pas:304-313) - idiom B, array-indexed. ---- */
static void emit_externs(Str *out, int indent)
{
    CTLExternList *list = sintactic_externs();
    size_t n = ctlextern_count(list);
    size_t i;

    emit_tabs(out, indent);
    str_append(out, "\"externs\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");
    for (i = 0; i < n; i++) {
        emit_tabs(out, indent);
        str_appendf(out, "{\"FilePath\":\"%s\"}", ctlextern_at(list, i));
        str_append(out, (i != n - 1) ? ",\r\n" : "\r\n");
    }
    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- vocabulary (UJSONExport.pas:32-36,316-323) - idiom A, IN-ORDER
   (ascending canonical key - NOT reversed, unlike symbols). ---- */
static void emit_vocabulary(Arena *a, Str *out, int indent)
{
    VocTree *tree = sintactic_voctree();
    Vec_VocEntry *entries = voctree_inorder(tree, a);
    size_t n = vec_len_VocEntry(entries);
    Str *body = str_new(a);
    size_t i;

    emit_tabs(out, indent);
    str_append(out, "\"vocabulary\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");

    for (i = 0; i < n; i++) {
        VocEntry *e = vec_at_VocEntry(entries, i);
        emit_tabs(body, indent);
        emit_tabs(body, indent);
        str_appendf(body, "{\"VocWord\":\"%s\", \"Value\":%ld,\"VocType\":%d },\n",
                    e->voc_word, e->value, (int)e->voc_type);
    }
    /* Same underflow guard as emit_symbols: an empty /VOC IS reachable
       (unlike symbols), and the reference emits exactly this shape. */
    if (str_len(body) >= 2)
        str_append_n(out, str_bytes(body), str_len(body) - 2);
    str_append(out, "\r\n");

    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- object_data (UJSONExport.pas:325-348) - idiom B. ---- */
static void emit_objects(Str *out, int indent)
{
    ObjectList *list = sintactic_objects();
    size_t n = objectlist_count(list);
    size_t i;

    emit_tabs(out, indent);
    str_append(out, "\"object_data\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");
    for (i = 0; i < n; i++) {
        const ObjectRecord *o = objectlist_at(list, i);
        emit_tabs(out, indent);
        str_append(out, "{\r\n");
        indent++;
        emit_tabs(out, indent); str_appendf(out, "\"Value\":%ld,\r\n", o->value);
        emit_tabs(out, indent); str_appendf(out, "\"Noun\":%ld,\r\n", o->noun);
        emit_tabs(out, indent); str_appendf(out, "\"Adjective\":%ld,\r\n", o->adjective);
        emit_tabs(out, indent); str_appendf(out, "\"Container\":%d,\r\n", o->container ? 1 : 0);
        emit_tabs(out, indent); str_appendf(out, "\"Wearable\":%d,\r\n", o->wearable ? 1 : 0);
        emit_tabs(out, indent); str_appendf(out, "\"Flags\":%u,\r\n", o->flags);
        emit_tabs(out, indent); str_appendf(out, "\"Weight\":%ld,\r\n", o->weight);
        emit_tabs(out, indent); str_appendf(out, "\"InitialyAt\":%ld\r\n", o->initially_at);
        indent--;
        emit_tabs(out, indent);
        str_append(out, "}");
        str_append(out, (i != n - 1) ? ",\r\n" : "\r\n");
    }
    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- connections (UJSONExport.pas:350-369) - idiom B. ---- */
static void emit_connections(Str *out, int indent)
{
    ConnectionList *list = sintactic_connections();
    size_t n = connectionlist_count(list);
    size_t i;

    emit_tabs(out, indent);
    str_append(out, "\"connections\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");
    for (i = 0; i < n; i++) {
        const ConnectionRecord *c = connectionlist_at(list, i);
        emit_tabs(out, indent);
        str_append(out, "{\r\n");
        indent++;
        emit_tabs(out, indent); str_appendf(out, "\"FromLoc\":%ld,\r\n", c->from_loc);
        emit_tabs(out, indent); str_appendf(out, "\"ToLoc\":%ld,\r\n", c->to_loc);
        emit_tabs(out, indent); str_appendf(out, "\"Direction\":%ld\r\n", c->direction);
        indent--;
        emit_tabs(out, indent);
        str_append(out, "}");
        str_append(out, (i != n - 1) ? ",\r\n" : "\r\n");
    }
    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- the six text tables (UJSONExport.pas:372-408) - idiom B. ---- */
static void emit_msg_table(Arena *a, Str *out, int indent, const char *key,
                            MsgList *list, int ascii7)
{
    size_t n = msglist_count(list);
    size_t i;

    emit_tabs(out, indent);
    str_appendf(out, "\"%s\":\r\n", key);
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");
    for (i = 0; i < n; i++) {
        const MsgEntry *m = msglist_at(list, i);
        Str *text = convert_text(a, m->text, ascii7);
        emit_tabs(out, indent);
        str_append(out, "{\r\n");
        indent++;
        emit_tabs(out, indent); str_appendf(out, "\"Value\":%ld,\r\n", m->id);
        emit_tabs(out, indent);
        str_append(out, "\"Text\":\"");
        str_append_n(out, str_bytes(text), str_len(text));
        str_append(out, "\"\r\n");
        indent--;
        emit_tabs(out, indent);
        str_append(out, "}");
        str_append(out, (i != n - 1) ? ",\r\n" : "\r\n");
    }
    emit_tabs(out, indent);
    str_append(out, "],\r\n");
}

/* ---- the process/entries/condacts walk (UJSONExport.pas:409-489) ---- */

/* PORT: the "Entry" derived string (UJSONExport.pas:427-438) - Verb
   tries VOC_VERB then, only if that misses AND
   Verb<=MAX_CONVERTIBLE_NAME, VOC_NOUN; Noun tries VOC_NOUN only. */
static const char *entry_verb_str(const VocTree *voc, long verb)
{
    VocEntry e;
    if (verb == NO_WORD) return "_";
    if (voctree_lookup_by_number(voc, verb, VOC_VERB, &e))
        return e.voc_word; /* stable: arena-owned by the voctree itself */
    if (verb <= MAX_CONVERTIBLE_NAME &&
        voctree_lookup_by_number(voc, verb, VOC_NOUN, &e))
        return e.voc_word;
    return "?";
}

static const char *entry_noun_str(const VocTree *voc, long noun)
{
    VocEntry e;
    if (noun == NO_WORD) return "_";
    if (voctree_lookup_by_number(voc, noun, VOC_NOUN, &e))
        return e.voc_word;
    return "?";
}

/* PORT: the Condact name resolution (UJSONExport.pas:450-453) - IsDB
   wins regardless of which directive produced the byte;
   then the two fake-condact codes; else the table name via
   condact_by_opcode (v3-aware, per Task 5's carry). A NULL table
   lookup here is an internal-invariant violation, not a Pascal
   behaviour to reproduce (every opcode reaching this point was
   validated at parse time - condact_lookup's own return, is_db's
   fixed literal, or one of the two fake codes) - fatal rather than
   silently emitting garbage. */
static const char *condact_name(Diag *d, int v3, const ProcessCondact *c)
{
    const CondactDef *def;

    if (c->is_db) return "#DB/#INCBIN";
    if (c->opcode == FAKE_USERPTR_CONDACT_CODE) return "#USERPTR";
    if (c->opcode == FAKE_DEBUG_CONDACT_CODE) return FAKE_DEBUG_CONDACT_TEXT;
    def = condact_by_opcode((int)c->opcode, v3);
    if (def == NULL) {
        diag_fatal(d, "internal error: opcode %ld has no condact definition",
                   c->opcode);
        return "";
    }
    return def->name;
}

static void emit_condacts(Str *out, int indent, Diag *d, int v3,
                           Vec_ProcessCondact *list)
{
    size_t n = vec_len_ProcessCondact(list);
    size_t i;

    emit_tabs(out, indent);
    str_append(out, "[\r\n");
    for (i = 0; i < n; i++) {
        const ProcessCondact *c = vec_at_ProcessCondact(list, i);
        int j;

        emit_tabs(out, indent);
        str_append(out, "{\r\n");
        indent++;
        emit_tabs(out, indent);
        str_appendf(out, "\"Opcode\":%ld,\r\n", c->opcode);
        emit_tabs(out, indent);
        str_appendf(out, "\"Condact\":\"%s\",\r\n", condact_name(d, v3, c));

        /* Two SEPARATE passes - all IndirectionN THEN all ParamN, not
           interleaved (UJSONExport.pas:455-462's two separate FOR
           loops - load-bearing for byte-identical emission). */
        if (c->num_params > 0) {
            for (j = 0; j < c->num_params; j++) {
                emit_tabs(out, indent);
                str_appendf(out, "\"Indirection%d\":%d,\r\n", j + 1,
                            c->params[j].indirection ? 1 : 0);
            }
            for (j = 0; j < c->num_params; j++) {
                emit_tabs(out, indent);
                str_appendf(out, "\"Param%d\":%ld,\r\n", j + 1,
                            c->params[j].value);
            }
        }
        emit_tabs(out, indent);
        str_appendf(out, "\"NumParams\":%d\r\n", c->num_params);

        indent--;
        emit_tabs(out, indent);
        str_append(out, "}");
        str_append(out, (i != n - 1) ? ",\r\n" : "\r\n");
    }
    emit_tabs(out, indent);
    str_append(out, "]\r\n");
}

static void emit_processes(Str *out, int indent, Diag *d,
                            const FrontOptions *opts)
{
    ProcessTable *procs = sintactic_processes();
    VocTree *voc = sintactic_voctree();
    long last = sintactic_last_process();
    long p;

    emit_tabs(out, indent);
    str_append(out, "\"processes\":\r\n");
    indent++;
    emit_tabs(out, indent);
    str_append(out, "[\r\n");

    for (p = 0; p <= last; p++) {
        const ProcessSlot *slot = processtable_get(procs, p);
        size_t ne, e;

        emit_tabs(out, indent);
        str_append(out, "{\r\n");
        indent++;
        emit_tabs(out, indent);
        str_appendf(out, "\"Value\":%ld,\r\n", slot->value);
        emit_tabs(out, indent);
        str_append(out, "\"entries\":\r\n");
        indent++;
        emit_tabs(out, indent);
        str_append(out, "[\r\n");

        ne = vec_len_ProcessEntry(slot->entries);
        for (e = 0; e < ne; e++) {
            const ProcessEntry *pe = vec_at_ProcessEntry(slot->entries, e);
            const char *verb_str = entry_verb_str(voc, pe->verb);
            const char *noun_str = entry_noun_str(voc, pe->noun);

            emit_tabs(out, indent);
            str_append(out, "{\r\n");
            indent++;
            emit_tabs(out, indent);
            str_appendf(out, "\"Entry\":\"%s %s\",\r\n", verb_str, noun_str);
            emit_tabs(out, indent);
            str_appendf(out, "\"Verb\":%ld,\r\n", pe->verb);
            emit_tabs(out, indent);
            str_appendf(out, "\"Noun\":%ld,\r\n", pe->noun);
            emit_tabs(out, indent);
            str_append(out, "\"condacts\":\r\n");
            indent++;
            emit_condacts(out, indent, d, opts->v3, pe->condacts);
            indent--;
            /* TRAILING DOUBLE DECREMENT (UJSONExport.pas:473-474's two
               DEC(Indent) calls): closes both the condacts array level
               and this entry object level together, before the
               entry's own closing brace. */
            indent--;
            emit_tabs(out, indent);
            str_append(out, "}");
            str_append(out, (e != ne - 1) ? ",\r\n" : "\r\n");
        }
        emit_tabs(out, indent);
        str_append(out, "]\r\n");
        indent--;
        /* Second trailing double decrement: closes the entries array
           level and this process object level together. */
        indent--;
        emit_tabs(out, indent);
        str_append(out, "}");
        str_append(out, (p != last) ? ",\r\n" : "\r\n");
    }

    emit_tabs(out, indent);
    str_append(out, "]\r\n"); /* processes: last top-level key, NO comma */
    indent--;
}

/* ---- GenerateJSON (UJSONExport.pas:268-495) ---- */

/* Builds the whole document in an internal working arena, copies the
   finished bytes into the CALLER'S arena `a`, frees the working arena
   before returning - see jsonexport.h for the lifetime contract. */
int jsonexport_render(Arena *a, Diag *d, const FrontOptions *opts,
                       const unsigned char **out_data, size_t *out_len)
{
    Arena *work = arena_new(0);
    Str *out = str_new(work);
    MessageList *msgs = sintactic_messages();
    int indent = 0;
    unsigned char *buf;

    emit_tabs(out, indent);
    str_append(out, "{\r\n");

    emit_settings(out, indent, opts);
    emit_symbols(work, out, indent);
    emit_externs(out, indent);
    emit_vocabulary(work, out, indent);
    emit_objects(out, indent);
    emit_connections(out, indent);

    emit_msg_table(work, out, indent, "messages", msgs->mtx, opts->ascii7);
    emit_msg_table(work, out, indent, "sysmess", msgs->stx, opts->ascii7);
    emit_msg_table(work, out, indent, "locations", msgs->ltx, opts->ascii7);
    emit_msg_table(work, out, indent, "objects", msgs->otx, opts->ascii7);
    emit_msg_table(work, out, indent, "xmessages", msgs->xtx, opts->ascii7);
    emit_msg_table(work, out, indent, "other_strings", msgs->other_tx, opts->ascii7);

    emit_processes(out, indent, d, opts);

    emit_tabs(out, indent);
    str_append(out, "}\r\n");

    *out_len = str_len(out);
    buf = arena_alloc(a, *out_len);
    memcpy(buf, str_bytes(out), *out_len);
    *out_data = buf;

    arena_free(work);
    return 0;
}

/* Thin wrapper: render into a scratch arena, one fwrite, free -
   byte-identical to the pre-seam single-Str/single-fwrite shape. */
int jsonexport_write(Diag *d, const char *path, const FrontOptions *opts)
{
    Arena *a = arena_new(0);
    const unsigned char *data;
    size_t len;
    FILE *f;

    jsonexport_render(a, d, opts, &data, &len);

    f = fopen(path, "wb");
    if (f == NULL) {
        diag_fatal(d, "cannot open \"%s\" for writing", path);
        arena_free(a);
        return diag_exit_code(d);
    }
    fwrite(data, 1, len, f);
    if (fclose(f) != 0) {
        diag_fatal(d, "error writing \"%s\"", path);
        arena_free(a);
        return diag_exit_code(d);
    }

    arena_free(a);
    return 0;
}
