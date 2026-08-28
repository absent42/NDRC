/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/lex_tokens.h - Copyright (C) 2026 Dan Gibson.

   PORT: ULexTokens.pas (D:/DRC/src, branch nextdaad) verbatim - the
   token id constants lexer.pas's 43 DSF.l actions pass to AddToken.
   Same names, same numeric values, same declaration order as the
   Pascal source; nothing renamed or renumbered. T_COMMENT and
   T_NOTHING are never actually produced by any of DSF.l's 43 rules
   (section 15.1 quirk (g): the comment rule discards its match and
   emits no token at all) but are ported anyway for verbatim fidelity
   to the constant list itself. */
#ifndef NDRC_FRONT_LEX_TOKENS_H
#define NDRC_FRONT_LEX_TOKENS_H

#define T_SECTION_CON 257
#define T_SECTION_CTL 258
#define T_SECTION_STX 259
#define T_SECTION_MTX 260
#define T_SECTION_LTX 261
#define T_SECTION_OTX 262
#define T_SECTION_OBJ 263
#define T_SECTION_VOC 264
#define T_SECTION_PRO 265
#define T_DEFINE 266
#define T_STRING 267
#define T_UNDERSCORE 268
#define T_LIST_ENTRY 269
#define T_COMMENT 270
#define T_IDENTIFIER 271
#define T_INDIRECT 272
#define T_NUMBER 273
#define T_IFDEF 274
#define T_IFNDEF 275
#define T_ENDIF 276
#define T_SECTION_END 277
#define T_DB 278
#define T_INCBIN 279
#define T_EXTERN 280
#define T_CLASSIC 281
#define T_ELSE 282
#define T_DW 283
#define T_ECHO 284
#define T_USERPTR 285
#define T_INT 286
#define T_SFX 287
#define T_NOTHING 288
#define T_HEX 289
#define T_DEBUG 290
#define T_PROCESS_ENTRY_SIGN 291
#define T_LABEL 292

#endif /* NDRC_FRONT_LEX_TOKENS_H */
