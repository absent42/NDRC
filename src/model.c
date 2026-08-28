/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/model.c - Adventure model, built from the JSON DOM.
   Copyright (C) 2026 Dan Gibson.

   Ported from drb.php replaceChars, replaceEscapeChars, checkStrings
   (drb.php:326-416), plus the extvec zeroing and settings extraction
   drb.php performs inline in its MAIN section (drb.php:1783-1801). The
   JSON walk itself (model_from_json's schema traversal) is new code: DRB
   has no equivalent, since PHP's json_decode hands it an untyped object
   graph it reads with unchecked property access.

   PORT NOTE: drb.php:326-389 (replaceChars) matches literal UTF-8 byte
   sequences for accented characters in the PHP source; json.c expands
   high bytes to the same UTF-8 pairs, so this port matches by Unicode
   code point instead (see conv/newconv below). */
#include "model.h"

#include <string.h>

/* ===================================================================
   JSON path helpers: build "table[i]", "table[i].field[j]" style paths
   incrementally during the walk (per the brief), so a schema violation
   names the offending field precisely, e.g.
   "processes[3].entries[1].condacts[0].Opcode". */

static const char *idxp(Arena *a, const char *name, size_t i)
{
    Str *s = str_new(a);
    str_appendf(s, "%s[%zu]", name, i);
    return str_cstr(s);
}

static const char *subidxp(Arena *a, const char *parent, const char *field, size_t i)
{
    Str *s = str_new(a);
    str_appendf(s, "%s.%s[%zu]", parent, field, i);
    return str_cstr(s);
}

static const char *joinp(Arena *a, const char *base, const char *key)
{
    Str *s = str_new(a);
    if (base[0] != '\0') {
        str_append(s, base);
        str_push(s, '.');
    }
    str_append(s, key);
    return str_cstr(s);
}

/* ===================================================================
   Typed JSON field accessors. base is the path of the ENCLOSING object
   (e.g. "processes[3].entries[1].condacts[0]"); key is the field name.
   A required field that is missing or of the wrong JSON type reports
   the full path via diag_fatal and returns the caller's default; an
   optional field does the same for wrong-type, but is silent (and
   returns dflt) when merely absent - matching PHP's undefined-property
   read (brief: "Absent optional condact fields ... default to 0/NULL
   exactly as PHP's undefined-property reads do"). */

static long get_int(Diag *d, Arena *a, const JsonValue *obj, const char *base,
                     const char *key, int required, long dflt)
{
    const JsonValue *v = json_get(obj, key);

    if (v == NULL) {
        if (required)
            diag_fatal(d, "invalid JSON input: %s missing", joinp(a, base, key));
        return dflt;
    }
    if (v->type != JSON_NUMBER) {
        diag_fatal(d, "invalid JSON input: %s has wrong type", joinp(a, base, key));
        return dflt;
    }
    return v->num;
}

static const char *get_cstr(Diag *d, Arena *a, const JsonValue *obj, const char *base,
                             const char *key, int required, const char *dflt)
{
    const JsonValue *v = json_get(obj, key);

    if (v == NULL) {
        if (required)
            diag_fatal(d, "invalid JSON input: %s missing", joinp(a, base, key));
        return dflt;
    }
    if (v->type != JSON_STRING) {
        diag_fatal(d, "invalid JSON input: %s has wrong type", joinp(a, base, key));
        return dflt;
    }
    return arena_strndup(a, v->str, v->str_len);
}

/* Text is binary-safe (str_len may exceed the first embedded NUL), so it
   goes into a Str via str_append_n rather than through arena_strndup. */
static Str *get_text(Diag *d, Arena *a, const JsonValue *obj, const char *base,
                      const char *key)
{
    const JsonValue *v = json_get(obj, key);
    Str *s = str_new(a);

    if (v == NULL) {
        diag_fatal(d, "invalid JSON input: %s missing", joinp(a, base, key));
        return s;
    }
    if (v->type != JSON_STRING) {
        diag_fatal(d, "invalid JSON input: %s has wrong type", joinp(a, base, key));
        return s;
    }
    str_append_n(s, v->str, v->str_len);
    return s;
}

/* Required array field; NULL (with a diagnostic already reported) if
   absent or of the wrong type. Callers must check for NULL before
   iterating, mirroring json.c's fail-fast pattern. */
static const JsonValue *get_array(Diag *d, Arena *a, const JsonValue *obj,
                                   const char *base, const char *key)
{
    const JsonValue *v = json_get(obj, key);

    if (v == NULL) {
        diag_fatal(d, "invalid JSON input: %s missing", joinp(a, base, key));
        return NULL;
    }
    if (v->type != JSON_ARRAY) {
        diag_fatal(d, "invalid JSON input: %s has wrong type", joinp(a, base, key));
        return NULL;
    }
    return v;
}

/* Validates an array element is an object (e.g. messages[i], not a bare
   number); NULL (with a diagnostic already reported) otherwise. */
static const JsonValue *elem_obj(Diag *d, const JsonValue *item, const char *path)
{
    if (item == NULL || item->type != JSON_OBJECT) {
        diag_fatal(d, "invalid JSON input: %s has wrong type", path);
        return NULL;
    }
    return item;
}

/* ===================================================================
   PORT: drb.php:326-389 replaceChars / replaceEscapeChars. */

/* Non-overlapping left-to-right replace-all over raw bytes, matching
   PHP's str_replace: the scan advances past a MATCHED search string by
   its own length (not re-scanning the just-emitted replacement), and
   otherwise advances one byte at a time. Returns a fresh Str; the input
   is never mutated, matching PHP's string value semantics ($str is
   reassigned, not edited in place - the caller relies on this to keep
   originalText a distinct, untouched buffer after Text is replaced). */
static Str *bytes_replace_all(Arena *a, const Str *in, const void *search,
                               size_t search_len, const void *repl, size_t repl_len)
{
    Str *out = str_new(a);
    const unsigned char *data = str_bytes(in);
    size_t len = str_len(in);
    size_t i = 0;

    if (search_len == 0) {
        str_append_n(out, data, len);
        return out;
    }
    while (i < len) {
        if (i + search_len <= len && memcmp(data + i, search, search_len) == 0) {
            str_append_n(out, repl, repl_len);
            i += search_len;
        } else {
            str_push(out, (char)data[i]);
            i++;
        }
    }
    return out;
}

static int bytes_contains(const Str *s, const void *needle, size_t needle_len)
{
    const unsigned char *data = str_bytes(s);
    size_t len = str_len(s);
    size_t i;

    if (needle_len == 0) return 1;
    if (needle_len > len) return 0;
    for (i = 0; i + needle_len <= len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

/* drb.php:364 is a bare echo with no newline (\g is not a PHP escape;
   live: two firings share one stdout line) - hence diag_note_raw. */
static void warn_old_escape_once(Diag *d, int *warned)
{
    if (*warned) return;
    diag_note_raw(d,
        "Warning: DRC does not support escape sequences with backslash "
        "character, use sharp (#) instead. i.e: #g instead of \\g");
    *warned = 1;
}

/* PORT: drb.php:326 replaceChars, ported byte-for-byte. drb.php:249's
   newConversions table has no entry for the uppercase accented vowels
   (only lowercase: e.g. 'è' at key 20, never 'È') - a DRC gap this port
   reproduces rather than fixes, per the porting protocol: an uppercase
   accented byte passes through untouched and is later caught by
   checkStrings as an invalid character, exactly as it is in DRB. */
static Str *daad_replace_chars(Arena *a, Diag *d, const Str *in)
{
    /* drb.php:248 conversions: index i -> output byte chr(i+16). Values
       are the two-byte UTF-8 encoding of each accented character, per
       the file header PORT NOTE. */
    static const struct { unsigned char b0, b1, out; } conv[16] = {
        {0xC2, 0xAA, 16}, {0xC2, 0xA1, 17}, {0xC2, 0xBF, 18}, {0xC2, 0xAB, 19},
        {0xC2, 0xBB, 20}, {0xC3, 0xA1, 21}, {0xC3, 0xA9, 22}, {0xC3, 0xAD, 23},
        {0xC3, 0xB3, 24}, {0xC3, 0xBA, 25}, {0xC3, 0xB1, 26}, {0xC3, 0x91, 27},
        {0xC3, 0xA7, 28}, {0xC3, 0x87, 29}, {0xC3, 0xBC, 30}, {0xC3, 0x9C, 31}
    };
    /* drb.php:249 newConversions: UTF-8 pair -> replaced with
       "#g" + chr(key) + "#t". */
    static const struct { unsigned char b0, b1, key; } newconv[17] = {
        {0xC3, 0xA0, 16}, {0xC3, 0xA3, 17}, {0xC3, 0xA4, 18}, {0xC3, 0xA2, 19},
        {0xC3, 0xA8, 20}, {0xC3, 0xAB, 21}, {0xC3, 0xAA, 22}, {0xC3, 0xAC, 23},
        {0xC3, 0xAF, 24}, {0xC3, 0xAE, 25}, {0xC3, 0xB2, 26}, {0xC3, 0xB5, 27},
        {0xC3, 0xB6, 28}, {0xC3, 0xB4, 29}, {0xC3, 0xB9, 30}, {0xC3, 0xBB, 31},
        {0xC3, 0x9F, 35}
    };
    /* drb.php:351-353 escape map, in the load-bearing order the brief
       specifies: #g #t #b #s #f #k #n #r, then #A..#P. */
    static const struct { char c1; unsigned char out; } esc[24] = {
        {'g', 0x0E}, {'t', 0x0F}, {'b', 0x0B}, {'s', 0x20},
        {'f', 0x7F}, {'k', 0x0C}, {'n', 0x0D}, {'r', 0x0D},
        {'A', 0x10}, {'B', 0x11}, {'C', 0x12}, {'D', 0x13},
        {'E', 0x14}, {'F', 0x15}, {'G', 0x16}, {'H', 0x17},
        {'I', 0x18}, {'J', 0x19}, {'K', 0x1A}, {'L', 0x1B},
        {'M', 0x1C}, {'N', 0x1D}, {'O', 0x1E}, {'P', 0x1F}
    };
    Str *cur = str_new(a);
    size_t i;
    int warned = 0;                 /* per-message: drb.php:355 is a local */

    str_append_n(cur, str_bytes(in), str_len(in));

    for (i = 0; i < sizeof(conv) / sizeof(conv[0]); i++) {
        unsigned char pair[2];
        pair[0] = conv[i].b0;
        pair[1] = conv[i].b1;
        cur = bytes_replace_all(a, cur, pair, 2, &conv[i].out, 1);
    }

    for (i = 0; i < sizeof(newconv) / sizeof(newconv[0]); i++) {
        unsigned char pair[2];
        unsigned char rep[5];
        pair[0] = newconv[i].b0;
        pair[1] = newconv[i].b1;
        rep[0] = '#'; rep[1] = 'g'; rep[2] = newconv[i].key; rep[3] = '#'; rep[4] = 't';
        cur = bytes_replace_all(a, cur, pair, 2, rep, 5);
    }

    for (i = 0; i < sizeof(esc) / sizeof(esc[0]); i++) {
        char search[2];
        search[0] = '#';
        search[1] = esc[i].c1;
        /* drb.php:359: the backslash check excludes '#n' specifically. */
        if (esc[i].c1 != 'n') {
            char oldseq[2];
            oldseq[0] = '\\';
            oldseq[1] = esc[i].c1;
            if (bytes_contains(cur, oldseq, 2)) warn_old_escape_once(d, &warned);
        }
        cur = bytes_replace_all(a, cur, search, 2, &esc[i].out, 1);
    }

    {
        /* drb.php:372: chr(10) -> chr(13). */
        unsigned char lf = 0x0A, cr = 0x0D;
        cur = bytes_replace_all(a, cur, &lf, 1, &cr, 1);
    }
    {
        /* drb.php:374: '##' -> '#', LAST, so a literal '#' survives. */
        static const unsigned char hh[2] = { '#', '#' };
        static const unsigned char h1[1] = { '#' };
        cur = bytes_replace_all(a, cur, hh, 2, h1, 1);
    }

    return cur;
}

/* PORT: drb.php:378 replaceEscapeChars. Runs over messages, sysmess,
   locations, objects, xmessages (NOT other_strings - drb.php:380's
   $tables array never names it). */
static void model_replace_escape_chars(Diag *d, Arena *a, Adventure *adv)
{
    Vec_Message *tables[5];
    int t;

    tables[0] = adv->messages;
    tables[1] = adv->sysmess;
    tables[2] = adv->locations;
    tables[3] = adv->objects;
    tables[4] = adv->xmessages;

    for (t = 0; t < 5; t++) {
        Vec_Message *table = tables[t];
        size_t i;
        for (i = 0; i < vec_len_Message(table); i++) {
            Message *m = vec_at_Message(table, i);
            m->originalText = m->Text;
            m->Text = daad_replace_chars(a, d, m->originalText);
        }
    }
}

/* PORT: drb.php:391 checkStrings. NOT xmessages (analysis S16.3) - only
   the four tables drb.php:393 names. Stops at the first offending byte,
   matching DRC halting at its first error. */
static void model_check_strings(Diag *d, const Adventure *adv)
{
    static const char *table_names[4] = {
        "user messages (MXT)", "system messages(STX)",
        "location texts(LTX)", "object texts(OTX)"
    };
    static const char *message_names[4] = { "message", "message", "location", "object" };
    Vec_Message *tables[4];
    int t;

    tables[0] = adv->messages;
    tables[1] = adv->sysmess;
    tables[2] = adv->locations;
    tables[3] = adv->objects;

    for (t = 0; t < 4; t++) {
        Vec_Message *table = tables[t];
        size_t msg_id;
        for (msg_id = 0; msg_id < vec_len_Message(table); msg_id++) {
            Message *m = vec_at_Message(table, msg_id);
            const unsigned char *bytes = str_bytes(m->Text);
            size_t len = str_len(m->Text);
            size_t i;
            for (i = 0; i < len; i++) {
                if (bytes[i] > 127) {
                    diag_fatal(d,
                        "Invalid character in %s, %s #%zu (%zu,#%u): '%s'",
                        table_names[t], message_names[t], msg_id, i + 1,
                        (unsigned)bytes[i], str_cstr(m->originalText));
                    return;
                }
            }
        }
    }
}

/* Every accessor above can call diag_fatal - including the "optional"
   ones, on a wrong-type value - so mfailed() is checked after EVERY
   field read within an object, not just once per array element: two
   sibling required fields both being absent (or one absent and the
   next mistyped) must report exactly one Error: line, not two. */
static int mfailed(const Diag *d, int base_errors)
{
    return diag_error_count(d) > base_errors;
}

static Vec_Message *load_message_table(Diag *d, Arena *a, const JsonValue *root,
                                        const char *name, int base_errors)
{
    const JsonValue *arr = get_array(d, a, root, "", name);
    Vec_Message *out = vec_new_Message(a);
    size_t i;

    if (arr == NULL) return out;
    for (i = 0; i < vec_len_JsonValue(arr->items); i++) {
        const char *path = idxp(a, name, i);
        const JsonValue *item = elem_obj(d, vec_at_JsonValue(arr->items, i), path);
        Message *m;

        if (item == NULL) return out;
        m = arena_calloc(a, sizeof(*m));
        m->Value = get_int(d, a, item, path, "Value", 1, 0);
        if (mfailed(d, base_errors)) return out;
        m->Text = get_text(d, a, item, path, "Text");
        if (mfailed(d, base_errors)) return out;
        m->originalText = NULL;
        vec_push_Message(out, m);
    }
    return out;
}

/* PORT: drb.php:109-113's FilePath split, ported to run at JSON-parse
   time (model.h's ExternEntry) instead of drb.php's own emit-time split
   inside generateExterns. explode('|', $externData): parts[0] is kept as
   the path regardless of how many further '|' the string contains;
   parts[1], if present, is only the text between the FIRST and SECOND
   '|' (PHP explode semantics, not "everything after the first '|'") -
   ported exactly rather than approximated, even though no fixture's
   FilePath carries a second '|'. sizeof($parts)<2 (no '|' at all)
   defaults parts[1] to 'EXTERN' (drb.php:111). */
static void split_extern_file_path(Arena *a, const char *combined,
                                    const char **file_path, const char **file_type)
{
    const char *bar1 = strchr(combined, '|');

    if (bar1 == NULL) {
        *file_path = combined;
        *file_type = "EXTERN";
        return;
    }
    {
        const char *bar2 = strchr(bar1 + 1, '|');
        size_t type_len = bar2 ? (size_t)(bar2 - (bar1 + 1)) : strlen(bar1 + 1);
        *file_path = arena_strndup(a, combined, (size_t)(bar1 - combined));
        *file_type = arena_strndup(a, bar1 + 1, type_len);
    }
}

/* PORT: drb.php's "externs" array (UJSONExport.pas:304-312, each entry
   {"FilePath":"<string>"}), walked the same way load_message_table walks
   its own array. */
static Vec_ExternEntry *load_externs(Diag *d, Arena *a, const JsonValue *root,
                                      int base_errors)
{
    const JsonValue *arr = get_array(d, a, root, "", "externs");
    Vec_ExternEntry *out = vec_new_ExternEntry(a);
    size_t i;

    if (arr == NULL) return out;
    for (i = 0; i < vec_len_JsonValue(arr->items); i++) {
        const char *path = idxp(a, "externs", i);
        const JsonValue *item = elem_obj(d, vec_at_JsonValue(arr->items, i), path);
        ExternEntry *e;
        const char *combined;

        if (item == NULL) return out;
        combined = get_cstr(d, a, item, path, "FilePath", 1, NULL);
        if (mfailed(d, base_errors)) return out;
        e = arena_calloc(a, sizeof(*e));
        split_extern_file_path(a, combined, &e->file_path, &e->file_type);
        vec_push_ExternEntry(out, e);
    }
    return out;
}

Adventure *model_from_json(Arena *a, Diag *d, const JsonValue *root)
{
    int base_errors = diag_error_count(d);
    Adventure *adv = arena_calloc(a, sizeof(*adv));
    const JsonValue *settings_arr, *settings0;
    const JsonValue *voc_arr, *objdata_arr, *conn_arr, *proc_arr;
    size_t i;

    /* ---- settings[0]: classic_mode, debug_mode, v3code, maluva_used ---- */
    settings_arr = get_array(d, a, root, "", "settings");
    if (settings_arr == NULL) return NULL;
    if (vec_len_JsonValue(settings_arr->items) < 1) {
        diag_fatal(d, "invalid JSON input: %s missing", "settings[0]");
        return NULL;
    }
    settings0 = elem_obj(d, vec_at_JsonValue(settings_arr->items, 0), "settings[0]");
    if (settings0 == NULL) return NULL;
    adv->classic_mode = (int)get_int(d, a, settings0, "settings[0]", "classic_mode", 1, 0);
    if (mfailed(d, base_errors)) return NULL;
    adv->debug_mode = (int)get_int(d, a, settings0, "settings[0]", "debug_mode", 1, 0);
    if (mfailed(d, base_errors)) return NULL;
    adv->v3code = (int)get_int(d, a, settings0, "settings[0]", "v3code", 1, 0);
    if (mfailed(d, base_errors)) return NULL;
    adv->maluva_used = (int)get_int(d, a, settings0, "settings[0]", "maluva_used", 1, 0);
    if (mfailed(d, base_errors)) return NULL;

    /* ---- symbols: never consumed (drb.php never reads it) - skipped. ----
       ---- externs (task-3-brief.md): loaded and split per-entry into
       ExternEntry - see load_externs and model.h. ---- */
    adv->externs = load_externs(d, a, root, base_errors);
    if (mfailed(d, base_errors)) return NULL;

    /* ---- vocabulary (JSON/sorted order preserved - do not reorder) ---- */
    voc_arr = get_array(d, a, root, "", "vocabulary");
    if (voc_arr == NULL) return NULL;
    adv->vocabulary = vec_new_VocWordEntry(a);
    for (i = 0; i < vec_len_JsonValue(voc_arr->items); i++) {
        const char *path = idxp(a, "vocabulary", i);
        const JsonValue *item = elem_obj(d, vec_at_JsonValue(voc_arr->items, i), path);
        VocWordEntry *w;

        if (item == NULL) return NULL;
        w = arena_calloc(a, sizeof(*w));
        w->VocWord = get_cstr(d, a, item, path, "VocWord", 1, NULL);
        if (mfailed(d, base_errors)) return NULL;
        w->Value = get_int(d, a, item, path, "Value", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        w->VocType = get_int(d, a, item, path, "VocType", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        vec_push_VocWordEntry(adv->vocabulary, w);
    }

    /* ---- object_data ---- */
    objdata_arr = get_array(d, a, root, "", "object_data");
    if (objdata_arr == NULL) return NULL;
    adv->object_data = vec_new_ObjectData(a);
    for (i = 0; i < vec_len_JsonValue(objdata_arr->items); i++) {
        const char *path = idxp(a, "object_data", i);
        const JsonValue *item = elem_obj(d, vec_at_JsonValue(objdata_arr->items, i), path);
        ObjectData *o;

        if (item == NULL) return NULL;
        o = arena_calloc(a, sizeof(*o));
        o->Value = get_int(d, a, item, path, "Value", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->Noun = get_int(d, a, item, path, "Noun", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->Adjective = get_int(d, a, item, path, "Adjective", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->Container = get_int(d, a, item, path, "Container", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->Wearable = get_int(d, a, item, path, "Wearable", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->Flags = get_int(d, a, item, path, "Flags", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->Weight = get_int(d, a, item, path, "Weight", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        o->InitialyAt = get_int(d, a, item, path, "InitialyAt", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        vec_push_ObjectData(adv->object_data, o);
    }

    /* ---- connections ---- */
    conn_arr = get_array(d, a, root, "", "connections");
    if (conn_arr == NULL) return NULL;
    adv->connections = vec_new_Connection(a);
    for (i = 0; i < vec_len_JsonValue(conn_arr->items); i++) {
        const char *path = idxp(a, "connections", i);
        const JsonValue *item = elem_obj(d, vec_at_JsonValue(conn_arr->items, i), path);
        Connection *c;

        if (item == NULL) return NULL;
        c = arena_calloc(a, sizeof(*c));
        c->FromLoc = get_int(d, a, item, path, "FromLoc", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        c->ToLoc = get_int(d, a, item, path, "ToLoc", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        c->Direction = get_int(d, a, item, path, "Direction", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        vec_push_Connection(adv->connections, c);
    }

    /* ---- message tables ---- */
    adv->messages = load_message_table(d, a, root, "messages", base_errors);
    if (mfailed(d, base_errors)) return NULL;
    adv->sysmess = load_message_table(d, a, root, "sysmess", base_errors);
    if (mfailed(d, base_errors)) return NULL;
    adv->locations = load_message_table(d, a, root, "locations", base_errors);
    if (mfailed(d, base_errors)) return NULL;
    adv->objects = load_message_table(d, a, root, "objects", base_errors);
    if (mfailed(d, base_errors)) return NULL;
    adv->xmessages = load_message_table(d, a, root, "xmessages", base_errors);
    if (mfailed(d, base_errors)) return NULL;
    adv->other_strings = load_message_table(d, a, root, "other_strings", base_errors);
    if (mfailed(d, base_errors)) return NULL;

    /* ---- processes ---- */
    proc_arr = get_array(d, a, root, "", "processes");
    if (proc_arr == NULL) return NULL;
    adv->processes = vec_new_Process(a);
    for (i = 0; i < vec_len_JsonValue(proc_arr->items); i++) {
        const char *ppath = idxp(a, "processes", i);
        const JsonValue *pitem = elem_obj(d, vec_at_JsonValue(proc_arr->items, i), ppath);
        const JsonValue *entries_arr;
        Process *proc;
        size_t j;

        if (pitem == NULL) return NULL;
        proc = arena_calloc(a, sizeof(*proc));
        proc->Value = get_int(d, a, pitem, ppath, "Value", 1, 0);
        if (mfailed(d, base_errors)) return NULL;
        entries_arr = get_array(d, a, pitem, ppath, "entries");
        if (entries_arr == NULL) return NULL;
        proc->entries = vec_new_ProcEntry(a);

        for (j = 0; j < vec_len_JsonValue(entries_arr->items); j++) {
            const char *epath = subidxp(a, ppath, "entries", j);
            const JsonValue *eitem = elem_obj(d, vec_at_JsonValue(entries_arr->items, j), epath);
            const JsonValue *condacts_arr;
            ProcEntry *pe;
            size_t k;

            if (eitem == NULL) return NULL;
            pe = arena_calloc(a, sizeof(*pe));
            /* Entry is optional: brief-curated PHP-undefined-property
               default, even though UJSONExport.pas always writes it. A
               present-but-mistyped Entry can still diag_fatal, so this
               is checked too, not just the required fields below. */
            pe->Entry = get_cstr(d, a, eitem, epath, "Entry", 0, NULL);
            if (mfailed(d, base_errors)) return NULL;
            pe->Verb = get_int(d, a, eitem, epath, "Verb", 1, 0);
            if (mfailed(d, base_errors)) return NULL;
            pe->Noun = get_int(d, a, eitem, epath, "Noun", 1, 0);
            if (mfailed(d, base_errors)) return NULL;
            condacts_arr = get_array(d, a, eitem, epath, "condacts");
            if (condacts_arr == NULL) return NULL;
            pe->condacts = vec_new_Condact(a);

            for (k = 0; k < vec_len_JsonValue(condacts_arr->items); k++) {
                const char *cpath = subidxp(a, epath, "condacts", k);
                const JsonValue *citem = elem_obj(d, vec_at_JsonValue(condacts_arr->items, k), cpath);
                Condact *co;

                if (citem == NULL) return NULL;
                co = arena_calloc(a, sizeof(*co));
                co->Opcode = get_int(d, a, citem, cpath, "Opcode", 1, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->NumParams = get_int(d, a, citem, cpath, "NumParams", 1, 0);
                if (mfailed(d, base_errors)) return NULL;
                /* Param1..4, Indirection1/2 and Condact are optional:
                   UJSONExport.pas only emits ParamN/IndirectionN for
                   N < NumParams, so a zero-parameter condact - the common
                   case - carries none of them at all (see model.h). Each
                   is still checked, since a mistyped (not merely absent)
                   value fatals too. */
                co->Param1 = get_int(d, a, citem, cpath, "Param1", 0, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->Param2 = get_int(d, a, citem, cpath, "Param2", 0, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->Param3 = get_int(d, a, citem, cpath, "Param3", 0, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->Param4 = get_int(d, a, citem, cpath, "Param4", 0, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->Indirection1 = get_int(d, a, citem, cpath, "Indirection1", 0, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->Indirection2 = get_int(d, a, citem, cpath, "Indirection2", 0, 0);
                if (mfailed(d, base_errors)) return NULL;
                co->Condact = get_cstr(d, a, citem, cpath, "Condact", 0, NULL);
                if (mfailed(d, base_errors)) return NULL;
                /* Not read from JSON at all - DRB sets this later, in the
                   pass-zero condact rewrite (drb.php:870-893), which is
                   out of Task 4's scope. */
                co->DurationAdjusted = 0;
                vec_push_Condact(pe->condacts, co);
            }
            vec_push_ProcEntry(proc->entries, pe);
            if (mfailed(d, base_errors)) return NULL;
        }
        vec_push_Process(adv->processes, proc);
        if (mfailed(d, base_errors)) return NULL;
    }

    /* drb.php:1783-1784 zeroes extvec before the two passes - order matched. */
    for (i = 0; i < 13; i++) adv->extvec[i] = 0;

    /* ---- PORT: the two remaining DRB input passes, drb.php:1786-1789 ---- */
    model_replace_escape_chars(d, a, adv);
    if (mfailed(d, base_errors)) return NULL;
    model_check_strings(d, adv);
    if (mfailed(d, base_errors)) return NULL;

    return adv;
}
