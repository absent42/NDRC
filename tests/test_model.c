/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tests/test_model.c - Copyright (C) 2026 Dan Gibson.

   Suite `model`: the JSON-DOM-to-Adventure walk, and the three ported
   DRB input passes (replaceEscapeChars/replaceChars, checkStrings,
   extvec zeroing). Diagnostics are asserted byte-exactly the same way
   test_diag.c does: redirect Diag's stream to a temp file and compare
   the captured bytes against DRC's real output shape. */
#include "test.h"
#include "arena.h"
#include "diag.h"
#include "json.h"
#include "model.h"
#include "str.h"
#include "vec.h"

#include <stdio.h>
#include <string.h>

static FILE *scratch_open(void)
{
    return tmpfile();
}

static void scratch_read(FILE *f, char *buf, size_t n)
{
    size_t got;
    rewind(f);
    got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
}

static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    const char *p = haystack;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

/* Assembles a full adventure JSON document from its per-table pieces, so
   each test only has to spell out the piece it cares about and can pass
   "[]" (or DEFAULT_SETTINGS) for the rest. */
static const char *DEFAULT_SETTINGS =
    "[{\"classic_mode\":0,\"debug_mode\":0,\"v3code\":0,\"maluva_used\":0}]";

static const char *make_json(Arena *a, const char *settings, const char *externs,
                              const char *vocabulary, const char *object_data,
                              const char *connections, const char *messages,
                              const char *sysmess, const char *locations,
                              const char *objects, const char *xmessages,
                              const char *other_strings, const char *processes)
{
    Str *s = str_new(a);
    str_appendf(s,
        "{\"settings\":%s,\"symbols\":[],\"externs\":%s,\"vocabulary\":%s,"
        "\"object_data\":%s,\"connections\":%s,\"messages\":%s,\"sysmess\":%s,"
        "\"locations\":%s,\"objects\":%s,\"xmessages\":%s,\"other_strings\":%s,"
        "\"processes\":%s}",
        settings, externs, vocabulary, object_data, connections,
        messages, sysmess, locations, objects, xmessages, other_strings, processes);
    return str_cstr(s);
}

/* Shorthand for the common case: only messages/processes/externs/etc.
   vary, everything else defaults to empty. */
static const char *make_json_default(Arena *a, const char *messages,
                                      const char *processes)
{
    return make_json(a, DEFAULT_SETTINGS, "[]", "[]", "[]", "[]",
                      messages, "[]", "[]", "[]", "[]", "[]", processes);
}

static const JsonValue *parse_ok(Arena *a, const char *json)
{
    JsonResult r = json_parse(a, (const unsigned char *)json, strlen(json));
    if (!r.ok) {
        printf("test JSON failed to parse: %s (line %d col %d)\n",
               r.err, r.line, r.col);
    }
    return r.root;
}

TEST(model_settings_extracted)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *json = make_json(a,
        "[{\"classic_mode\":1,\"debug_mode\":0,\"v3code\":1,\"maluva_used\":1}]",
        "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));

    CHECK(adv != NULL);
    CHECK_INT(adv->classic_mode, 1);
    CHECK_INT(adv->debug_mode, 0);
    CHECK_INT(adv->v3code, 1);
    CHECK_INT(adv->maluva_used, 1);
    CHECK_INT(diag_error_count(d), 0);

    arena_free(a);
}

TEST(model_counts_match_literal_json)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *json = make_json(a, DEFAULT_SETTINGS, "[]", "[]",
        "[{\"Value\":0,\"Noun\":1,\"Adjective\":255,\"Container\":0,"
          "\"Wearable\":0,\"Flags\":0,\"Weight\":1,\"InitialyAt\":2}]",
        "[{\"FromLoc\":0,\"ToLoc\":1,\"Direction\":1}]",
        "[{\"Value\":0,\"Text\":\"a\"},{\"Value\":1,\"Text\":\"b\"}]",
        "[]",
        "[{\"Value\":0,\"Text\":\"loc1\"},{\"Value\":1,\"Text\":\"loc2\"}]",
        "[{\"Value\":0,\"Text\":\"obj\"}]",
        "[]", "[]", "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(vec_len_Message(adv->messages), 2);
    CHECK_INT(vec_len_Message(adv->locations), 2);
    CHECK_INT(vec_len_Message(adv->objects), 1);
    CHECK_INT(vec_len_ObjectData(adv->object_data), 1);
    CHECK_INT(vec_len_Connection(adv->connections), 1);
    CHECK_INT(vec_len_Message(adv->sysmess), 0);
    CHECK_INT(vec_len_Message(adv->xmessages), 0);
    CHECK_INT(vec_len_Process(adv->processes), 0);

    arena_free(a);
}

TEST(model_missing_opcode_reports_full_path)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *processes =
        "[{\"Value\":0,\"entries\":[{\"Entry\":\"GO NORTH\",\"Verb\":1,"
        "\"Noun\":2,\"condacts\":[{\"Condact\":\"SCORE\",\"NumParams\":0}]}]}]";
    const char *json = make_json_default(a, "[]", processes);
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));

    CHECK(adv == NULL);
    CHECK_INT(diag_error_count(d), 1);
    CHECK_STR(diag_last_error(d),
        "invalid JSON input: processes[0].entries[0].condacts[0].Opcode missing");

    arena_free(a);
}

TEST(model_high_byte_0xc8_triggers_checkstrings_message)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[512];
    /* "\u00C8" is DAAD text byte 0xC8 (uppercase E-grave), which json.c
       expands to its two-byte UTF-8 pair C3 88 per ruling P1 - the same
       pair drb.php's own utf8_encode+json_decode pipeline would produce.
       Neither conv[] nor newconv[] in daad_replace_chars covers it (DRB
       has no uppercase-vowel entry in newConversions - reproduced, not
       fixed), so it survives replaceChars untouched and checkStrings
       catches the leading C3 byte (195) at position 1. */
    const char *messages = "[{\"Value\":0,\"Text\":\"\\u00C8\"}]";
    const char *json = make_json_default(a, messages, "[]");
    Adventure *adv;

    diag_set_stream(d, f);
    adv = model_from_json(a, d, parse_ok(a, json));
    scratch_read(f, buf, sizeof(buf));

    CHECK(adv == NULL);
    CHECK_STR(buf,
        "Error: Invalid character in user messages (MXT), "
        "message #0 (1,#195): '\xC3\x88'.\n");

    fclose(f);
    arena_free(a);
}

TEST(model_double_hash_collapses_to_hash)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    /* "9" (not a letter) follows the pair deliberately: "a##b" would
       instead hit the "#b" escape-map entry first (its second '#' pairs
       with the trailing 'b'), consuming it as the #b=>0x0B escape before
       the "##"=>"#" rule ever runs - correct DRB behaviour, but not what
       this test means to exercise. */
    const char *messages = "[{\"Value\":0,\"Text\":\"x##9\"}]";
    const char *json = make_json_default(a, messages, "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Message *m;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    m = vec_at_Message(adv->messages, 0);
    CHECK_INT(str_len(m->Text), 3);
    CHECK_MEM(str_bytes(m->Text), "x#9", 3);

    arena_free(a);
}

TEST(model_hash_n_becomes_cr)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *messages = "[{\"Value\":0,\"Text\":\"#n\"}]";
    const char *json = make_json_default(a, messages, "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Message *m;

    CHECK(adv != NULL);
    m = vec_at_Message(adv->messages, 0);
    CHECK_INT(str_len(m->Text), 1);
    CHECK_INT((unsigned char)str_bytes(m->Text)[0], 0x0D);

    arena_free(a);
}

TEST(model_lf_becomes_cr)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    /* \u000A is a raw line-feed byte in the decoded Text, matching the
       "users writing \n [...] going through as chr(10)" case drb.php:371
       describes. */
    const char *messages = "[{\"Value\":0,\"Text\":\"\\u000A\"}]";
    const char *json = make_json_default(a, messages, "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Message *m;

    CHECK(adv != NULL);
    m = vec_at_Message(adv->messages, 0);
    CHECK_INT(str_len(m->Text), 1);
    CHECK_INT((unsigned char)str_bytes(m->Text)[0], 0x0D);

    arena_free(a);
}

TEST(model_originaltext_preserved_before_replacement)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *messages = "[{\"Value\":0,\"Text\":\"##\"}]";
    const char *json = make_json_default(a, messages, "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Message *m;

    CHECK(adv != NULL);
    m = vec_at_Message(adv->messages, 0);
    CHECK(m->originalText != NULL);
    CHECK_INT(str_len(m->originalText), 2);
    CHECK_MEM(str_bytes(m->originalText), "##", 2);
    CHECK_INT(str_len(m->Text), 1);
    CHECK_MEM(str_bytes(m->Text), "#", 1);

    arena_free(a);
}

TEST(model_extern_filepath_splits_path_and_type)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *json = make_json(a, DEFAULT_SETTINGS,
        "[{\"FilePath\":\"EXT_MAIN.BIN|EXTERN\"}]",
        "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    ExternEntry *e;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT((int)vec_len_ExternEntry(adv->externs), 1);
    e = vec_at_ExternEntry(adv->externs, 0);
    CHECK_STR(e->file_path, "EXT_MAIN.BIN");
    CHECK_STR(e->file_type, "EXTERN");

    arena_free(a);
}

/* PORT: drb.php:111 - "this is just to be able to process old version
   .JSON files", a FilePath with no '|' at all defaults to type
   EXTERN. */
TEST(model_extern_filepath_missing_bar_defaults_extern)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *json = make_json(a, DEFAULT_SETTINGS,
        "[{\"FilePath\":\"EXT_MAIN.BIN\"}]",
        "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    ExternEntry *e;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    e = vec_at_ExternEntry(adv->externs, 0);
    CHECK_STR(e->file_path, "EXT_MAIN.BIN");
    CHECK_STR(e->file_type, "EXTERN");

    arena_free(a);
}

TEST(model_xmessages_parse)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *json = make_json(a, DEFAULT_SETTINGS, "[]", "[]", "[]", "[]",
        "[]", "[]", "[]", "[]",
        "[{\"Value\":0,\"Text\":\"one\"},{\"Value\":1,\"Text\":\"two\"}]",
        "[]", "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Message *m0, *m1;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT((int)vec_len_Message(adv->xmessages), 2);
    m0 = vec_at_Message(adv->xmessages, 0);
    m1 = vec_at_Message(adv->xmessages, 1);
    CHECK_INT(str_len(m0->Text), 3);
    CHECK_MEM(str_bytes(m0->Text), "one", 3);
    CHECK_INT(str_len(m1->Text), 3);
    CHECK_MEM(str_bytes(m1->Text), "two", 3);

    arena_free(a);
}

TEST(model_xmessages_escape_chars_replaced)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    /* drb.php:371 - "#n" becomes a CR byte, already table 4 (model.c's
       model_replace_escape_chars, tables[4]=adv->xmessages) - same
       replaceEscapeChars pass proven for messages above
       (model_hash_n_becomes_cr), now asserted against xmessages. */
    const char *xmessages = "[{\"Value\":0,\"Text\":\"#n\"}]";
    const char *json = make_json(a, DEFAULT_SETTINGS, "[]", "[]", "[]", "[]",
        "[]", "[]", "[]", "[]", xmessages, "[]", "[]");
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Message *m;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    m = vec_at_Message(adv->xmessages, 0);
    CHECK_INT(str_len(m->Text), 1);
    CHECK_INT((unsigned char)str_bytes(m->Text)[0], 0x0D);

    arena_free(a);
}

TEST(model_two_simultaneous_defects_report_one_error_line)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[1024];
    /* settings[0] is missing BOTH classic_mode and debug_mode - two
       independent defects in the same object. Without short-circuiting
       sibling required-field reads, both would diag_fatal before the
       caller ever gets to check diag_error_count, printing two Error:
       lines instead of DRC's documented halt-at-first-error. */
    const char *json = make_json(a,
        "[{\"v3code\":0,\"maluva_used\":0}]",
        "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]", "[]");
    Adventure *adv;

    diag_set_stream(d, f);
    adv = model_from_json(a, d, parse_ok(a, json));
    scratch_read(f, buf, sizeof(buf));

    CHECK(adv == NULL);
    CHECK_INT(diag_error_count(d), 1);
    CHECK_STR(diag_last_error(d), "invalid JSON input: settings[0].classic_mode missing");
    CHECK_INT(count_occurrences(buf, "Error: "), 1);

    fclose(f);
    arena_free(a);
}

TEST(model_condact_optional_fields_default_to_zero_or_null)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *processes =
        "[{\"Value\":0,\"entries\":[{\"Verb\":1,\"Noun\":2,"
        "\"condacts\":[{\"Opcode\":5,\"NumParams\":0}]}]}]";
    const char *json = make_json_default(a, "[]", processes);
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Process *p;
    ProcEntry *pe;
    Condact *co;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    p = vec_at_Process(adv->processes, 0);
    pe = vec_at_ProcEntry(p->entries, 0);
    co = vec_at_Condact(pe->condacts, 0);
    CHECK_INT(co->Opcode, 5);
    CHECK_INT(co->NumParams, 0);
    CHECK_INT(co->Param1, 0);
    CHECK_INT(co->Param2, 0);
    CHECK_INT(co->Param3, 0);
    CHECK_INT(co->Param4, 0);
    CHECK_INT(co->Indirection1, 0);
    CHECK_INT(co->Indirection2, 0);
    CHECK(co->Condact == NULL);
    CHECK_INT(co->DurationAdjusted, 0);

    arena_free(a);
}

TEST(model_entry_missing_entry_field_defaults_null)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    const char *processes =
        "[{\"Value\":0,\"entries\":[{\"Verb\":1,\"Noun\":2,\"condacts\":[]}]}]";
    const char *json = make_json_default(a, "[]", processes);
    Adventure *adv = model_from_json(a, d, parse_ok(a, json));
    Process *p;
    ProcEntry *pe;

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    p = vec_at_Process(adv->processes, 0);
    pe = vec_at_ProcEntry(p->entries, 0);
    CHECK(pe->Entry == NULL);
    CHECK_INT(pe->Verb, 1);
    CHECK_INT(pe->Noun, 2);

    arena_free(a);
}

TEST(model_backslash_escape_warning_emitted_once)
{
    Arena *a = arena_new(0);
    Diag *d = diag_new(a);
    FILE *f = scratch_open();
    char buf[1024];
    /* JSON text "\\g\\t" decodes to the four raw bytes backslash,g,
       backslash,t: two DIFFERENT old-style escape sequences in one
       message, so the once-only gate (drb.php:355's $oldSequenceWarning
       is local to a single replaceChars call) must fire exactly once. */
    const char *messages = "[{\"Value\":0,\"Text\":\"\\\\g\\\\t\"}]";
    const char *json = make_json_default(a, messages, "[]");
    Adventure *adv;

    diag_set_stream(d, f);
    adv = model_from_json(a, d, parse_ok(a, json));
    scratch_read(f, buf, sizeof(buf));

    CHECK(adv != NULL);
    CHECK_INT(diag_error_count(d), 0);
    CHECK_INT(count_occurrences(buf, "DRC does not support escape sequences"), 1);

    fclose(f);
    arena_free(a);
}

int main(void)
{
    RUN(model_settings_extracted);
    RUN(model_counts_match_literal_json);
    RUN(model_missing_opcode_reports_full_path);
    RUN(model_high_byte_0xc8_triggers_checkstrings_message);
    RUN(model_double_hash_collapses_to_hash);
    RUN(model_hash_n_becomes_cr);
    RUN(model_lf_becomes_cr);
    RUN(model_originaltext_preserved_before_replacement);
    RUN(model_extern_filepath_splits_path_and_type);
    RUN(model_extern_filepath_missing_bar_defaults_extern);
    RUN(model_xmessages_parse);
    RUN(model_xmessages_escape_chars_replaced);
    RUN(model_two_simultaneous_defects_report_one_error_line);
    RUN(model_condact_optional_fields_default_to_zero_or_null);
    RUN(model_entry_missing_entry_field_defaults_null);
    RUN(model_backslash_escape_warning_emitted_once);
    return test_summary("model");
}
