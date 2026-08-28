/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/include.h - Copyright (C) 2026 Dan Gibson.

   PORT: UInclude.pas (D:/DRC/src, branch nextdaad) - the temp-line to
   original-file/line map, plus drf.pas's Preparse (drf.pas:154-208),
   the map's only writer. Preparse runs on EVERY compile, even with no
   #include present, which is also what guarantees the temp file's
   last line always ends in a newline (WriteLn per line), masking
   defect 19.46's lexer edge case in ordinary use. State resets at
   each preparse() entry so tests can run several compiles in one
   process. */
#ifndef NDRC_FRONT_INCLUDE_H
#define NDRC_FRONT_INCLUDE_H

#include "arena.h"
#include "diag.h"

/* PORT: TIncludeData (UInclude.pas:9-12), field for field. */
typedef struct IncludeData {
    long original_line;
    const char *original_filename;
} IncludeData;

/* Clears the map. preparse() calls this itself; exposed for tests and
   for any caller that lexes without a preparse stage. */
void include_reset(void);

/* PORT: AddLine (UInclude.pas:26-30). `main_line` is 1-based (the
   Pascal SetLengths the dynamic array to AMainLine and writes index
   AMainLine-1). The reference is only ever called with main_line =
   previous length + 1 (Preparse's TempLine counter); this port
   accepts exactly that and grows the map by one. `original_filename`
   is duplicated into `a`. */
void include_add_line(Arena *a, long main_line,
                       const char *original_filename, long original_line);

/* PORT: GetIncludeData (UInclude.pas:32-35). Returns 1 and fills *out
   for a mapped 1-based line.

   PORT NOTE (memory-safety guard, FIDELITY POLICY exception): the
   reference indexes the dynamic array with no bounds check ({$R+} is
   not set in UInclude.pas), so an out-of-range line - including
   GetIncludeData(0), reachable only by an error fired before any real
   token - reads heap garbage. This port returns 0 instead (out
   untouched); callers fall back to the unmapped line/source. */
int include_get_data(long main_line, IncludeData *out);

/* SyntaxError/Warning's GetIncludeData threading (USintactic.pas:
   34-47) for any caller reporting a diagnostic at a temp-file line:
   maps `line` through the include map and, on a hit, points diag's
   source name at the original file, returning the original line. On a
   miss (no preparse ran, or the guard above) leaves diag's source
   unchanged and returns `line` as-is. */
long include_remap_for_diag(Diag *d, long line);

/* PORT: Preparse (drf.pas:154-208). Reads `input_filename` line by
   line (FPC ReadLn semantics: CRLF and LF both end a line, a lone CR
   survives as a byte - same rules as lexlib.c's fetch_line), writes
   each line to `temp_filename` followed by a newline (WriteLn - the
   reference platform emits CRLF, pinned here like the JSON writer's
   line endings), and splices #include files:

     - a line is an include directive only when its FIRST 8 characters
       are literally `#include` (column 1, case-sensitive);
     - the filename is everything from character 10 on (character 9,
       whatever it is, is discarded as the separator), truncated at
       the first `;` and trimmed of bytes <= 0x20 at both ends; quotes
       are NOT stripped (a quoted name is looked up with its quotes
       and normally fails);
     - a missing include file: PreparseError `Include file "<name>"
       not found` at the MAIN file's current line, exit class 2;
     - `Including <name>...` is echoed per include under verbose;
     - a `#include` line inside an included file: PreparseError
       `Nested includes are not allowed` at the INCLUDE-local line.

   Every written temp line gets one include_add_line record naming its
   original file and line. Returns 1 on success; 0 after a
   PreparseError (already reported via diag, exit class 2). */
int preparse(Arena *a, Diag *d, const char *input_filename,
              const char *temp_filename);

#endif /* NDRC_FRONT_INCLUDE_H */
