/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/expr.h - Copyright (C) 2026 Dan Gibson.

   PORT: TFPExpressionParser (fpexprpars.pp, FPC 3.2.2) as DRF configures
   it in GetExpressionValue (USintactic.pas:114-139): a fresh parser per
   call, BuiltIns := [bcMath] (116-117), and every symbol registered via
   Identifiers.AddFloatVariable (118-123) - so every DSF symbol is a
   FLOAT variable here, whatever its stored Longint value. This unit is
   the ENGINE only: scanner, seven-level descent, node type checks,
   evaluator, exception texts verbatim - NOT the call site (quote
   stripping, ShortString truncation, trunc() of the result, the Longint
   range check, MaxLongInt sentinel collision, and the two error shells,
   USintactic.pas:130,137), which belong to the caller. EXPR_FAIL's msg
   is the inner text alone, no shell, no trailing period; the engine
   never calls diag or exits - everything comes back as an ExprResult. */
#ifndef NDRC_FRONT_EXPR_H
#define NDRC_FRONT_EXPR_H

#include <stdint.h>

#include "symbols.h"

typedef enum {
    EXPR_INT,     /* rtInteger result: out->ival */
    EXPR_FLOAT,   /* rtFloat result: out->fval */
    EXPR_NONNUM,  /* evaluated fine but boolean/string result */
    EXPR_FAIL     /* scanner/parser/evaluator exception: out->msg */
} ExprKind;

typedef struct {
    ExprKind kind;
    int64_t  ival;
    double   fval;
    char     msg[1024]; /* EXPR_FAIL only: the inner engine text, no
                           'Invalid expression' shell (the caller
                           composes it, USintactic.pas:130). 1024 not
                           512: type-mismatch texts render both operand
                           subtrees and can exceed the expression
                           length; still clipped by vsnprintf past
                           1023. */
} ExprResult;

/* Evaluates `text` (already quote-stripped by the caller) exactly as
   Parser.Expression := text; Parser.Evaluate would.

   `symbols` may be NULL (no variables registered); when it is not, every
   entry is registered as a float variable in symbols_at order, and a
   USER name wins a collision with a bcMath builtin
   (fpexprpars.pp:1526-1540 inserts builtins first, the probe pins the
   user entry as the one lookup returns).

   `a` must not be NULL: the parse tree and the AsString renderings that
   the node-type error texts embed are built in it. Nothing outlives the
   call - the caller is free to use a scratch arena and reset it.

   `out` is always filled. EXPR_NONNUM carries no msg (the caller's
   "returned a non numeric value" text is composed from its own copy of
   the expression); msg is meaningful for EXPR_FAIL alone. */
void expr_evaluate(const char *text, const SymbolList *symbols,
                   Arena *a, ExprResult *out);

#endif /* NDRC_FRONT_EXPR_H */
