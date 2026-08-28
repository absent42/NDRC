/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/diag.h - error, warning and progress reporting.
   Copyright (C) 2026 Dan Gibson.

   NDRC reproduces DRC's diagnostic output and exit codes exactly (owner
   ruling, 2026-08-25). Callers pass the message WITHOUT a trailing
   period; diag appends it, matching DRC.

   Five shapes are implemented:

   1. diag_syntax_error - syntax/lexical error (USintactic.pas:34-40),
      terminates compilation, records exit class 1:
          <line>:<col>:<file>: <message>.
      Confirmed live output: `2:13:g.DSF: "CAF" already defined.`

   2. diag_warn - warning (USintactic.pas:42-47), does not terminate:
          Warning: <line>:<col>:<file>: <message>.

   3. diag_fatal - back-end / fatal error (drb.php:1351-1355), records
      exit class 2. This is the shape every DRB error uses, and
      therefore the shape `ndrc --from-json` uses in Phase 1a:
          Error: <message>.

   4. diag_preparse_error - the Preparse stage's shape (drf.pas:56-60),
      records exit class 2, carries NO filename (live-verified against
      drf.exe: `1:0: Include file "missing_file.inc" not found.`):
          <line>:0: <message>.

   5. diag_param_error - ParamError (drf.pas:50-54), records exit class
      2, carries NO prefix at all - unlike diag_fatal's "Error: " lead-
      in, this shape is the bare message plus period, nothing else
      (live-verified against drf.exe: `Invalid option: -bogus.`,
      `Input file not found: "nosuchfile.dsf".`, both exit 2). This is
      drf.pas's own front-end CLI error shape, used by `ndrc --to-json`
      wherever the Pascal calls ParamError - never by `--from-json`,
      which has no ParamError equivalent (drb.php's Error() is
      diag_fatal's shape 3, unrelated).

   Exit codes: 0 success, 1 syntax/lexical, 2 parameter/file/back-end.
   DRC halts at the FIRST error, so the exit code is the class of the
   first error reported. diag_exit_code reports that class. diag itself
   never calls exit() - the caller decides when to stop - but
   byte-faithful behaviour means callers should stop at DRC's first
   error too.

   DRC's source-bearing shapes (1 and 2) always carry a filename - there
   is no no-source fallback for them. Calling diag_syntax_error or
   diag_warn before diag_set_source is a programming error: it reports
   to stderr and aborts, following the project's established guard
   pattern (vec_set out of range, str back-patch out of range). diag_fatal
   and diag_note carry no filename and are unaffected. */
#ifndef NDRC_DIAG_H
#define NDRC_DIAG_H

#include <stdio.h>
#include "arena.h"

typedef struct Diag Diag;

Diag *diag_new(Arena *a);

/* Sets the filename used in message prefixes. Not copied lazily: the
   string is duplicated into the arena. */
void diag_set_source(Diag *d, const char *filename);

void diag_set_verbose(Diag *d, int on);

/* Redirects output. Defaults to stderr. Tests point this at a temp file
   so they assert on the exact bytes a user would see. */
void diag_set_stream(Diag *d, FILE *out);

/* Shape 1: syntax/lexical error. Records exit class 1. Requires
   diag_set_source to have been called first: aborts otherwise. */
void diag_syntax_error(Diag *d, int line, int col, const char *fmt, ...);

/* Shape 2: warning. Does not affect diag_exit_code. Requires
   diag_set_source to have been called first: aborts otherwise. */
void diag_warn(Diag *d, int line, int col, const char *fmt, ...);

/* Shape 3: back-end / fatal error. No position - DRB's shape carries
   none. Records exit class 2. */
void diag_fatal(Diag *d, const char *fmt, ...);

/* Shape 4: Preparse-stage error (drf.pas:56-60). `<line>:0: <msg>.`,
   no filename, exit class 2. Does not require diag_set_source. */
void diag_preparse_error(Diag *d, long line, const char *fmt, ...);

/* Shape 5: ParamError (drf.pas:50-54). Bare `<msg>.`, no prefix, no
   position, no filename, exit class 2. Does not require
   diag_set_source. */
void diag_param_error(Diag *d, const char *fmt, ...);

/* Unconditional progress output, carrying no severity prefix and
   counting as neither error nor warning. */
void diag_note(Diag *d, const char *fmt, ...);

/* As diag_note, but WITHOUT the trailing newline diag_note always
   appends. Needed for the handful of DRB `echo` sites (e.g. drb.php:364)
   whose own literal carries no "\n" of its own - diag_note's newline
   would be an extra byte DRB never emits at that exact site. Counts as
   neither error nor warning, same as diag_note. */
void diag_note_raw(Diag *d, const char *fmt, ...);

/* As diag_note, but suppressed unless verbose is enabled. */
void diag_verbose(Diag *d, const char *fmt, ...);

int diag_error_count(const Diag *d);
int diag_warn_count(const Diag *d);

/* Text of the most recent error or warning, without prefix or trailing
   period. NULL if none has been reported. Used by tests and by the
   oracle harness. */
const char *diag_last(const Diag *d);

/* Text of the most recent ERROR only (diag_syntax_error or diag_fatal),
   without prefix or trailing period. NULL if no error has been
   reported. Unlike diag_last, a warning reported after an error does
   not change this. */
const char *diag_last_error(const Diag *d);

/* 0 if no error has been reported; otherwise the exit class of the
   FIRST error reported (1 or 2), matching DRC halting at its first
   error. */
int diag_exit_code(const Diag *d);

#endif /* NDRC_DIAG_H */
