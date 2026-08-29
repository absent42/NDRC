/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/backend.h - the DRB back-end stage, driven by a parsed option set
   rather than by argv.
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_BACKEND_H
#define NDRC_BACKEND_H

#include <stddef.h>
#include "arena.h"
#include "diag.h"

typedef struct {
    const char *target_arg;     /* raw argv token, NOT validated;
                                   NULL = absent (usage error) */
    const char *subtarget_arg;  /* NULL when absent */
    const char *lang_arg;       /* raw argv token; NULL = absent */
    const char *input_name;     /* the name drb echoes/derives from -
                                   the JSON-side name (see the PORT
                                   notes in backend.c for the
                                   dirname-prefix echo); NULL = absent */
    const char *output_path;    /* NULL = derive drb-style */
    /* drb-side option flags, exactly the locals of the old
       run_from_json: */
    int verbose, forced_classic, prepend_c64, prepend_plus3;
    int dump_to_xmb, forced_padding, forced_no_padding, forced_debug;
    long forced_base;           /* <0 = unset */
    int auto_tokens;            /* -auto-tokens: select per-game tokens */
    int tok_tee;                /* --tok seen (implies auto_tokens) */
    const char *tok_tee_path;   /* --tok=path; NULL = derive <input>.tok */
    /* The caller's option-loop verdict. drb reports
       parseOptionalParameters' errors from its call site
       (drb.php:1769), after the Target line and the JSON decode, so a
       caller that parses options itself records the message text here
       instead of printing it. NULL = no error; a non-NULL string is
       borrowed, and must outlive the backend_run call. */
    const char *option_error;
} BackendOptions;

/* The whole drb stage from "arguments known" onward: target/language
   validation (reference texts), JSON parse of (data,len), model
   build, emission, all stdout echoes, DDB write. Returns the exit
   code the old run_from_json returned. Never reads argv, and never
   prints an ndrc banner. json_data NULL = the caller's file read
   failed, reported at drb's own position as "File not found". */
int backend_run(Arena *a, Diag *d, const unsigned char *json_data,
                size_t json_len, const BackendOptions *opts);

/* NDRC's own --from-json usage text: one copy, shared with the CLI
   layer's own subcommand check. */
extern const char BACKEND_USAGE_MSG[];

#endif /* NDRC_BACKEND_H */
