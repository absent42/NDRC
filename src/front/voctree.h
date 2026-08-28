/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/voctree.h - Copyright (C) 2026 Dan Gibson.

   PORT: UVocabularyTree.pas (D:/DRC/src, branch nextdaad) - the
   vocabulary, an ordinary unbalanced binary search tree keyed by the
   CANONICALISED word text alone (analysis 26.1). Canonicalisation
   (`FixSpanishChars(AnsiUpperCase(word))`, UVocabularyTree.pas:75-79):
   ASCII (and full Latin-1, per str_upper_latin1) upper-case, then the
   eight uppercase-accented letters this port's FixSpanishChars mapping
   folds back DOWN to their lower-case Latin-1 byte (voctree.c has the
   script-diffed table) - net effect: ASCII upper, those eight accents
   lower. Every insert/lookup entry point folds internally; callers
   must NOT pre-fold. */
#ifndef NDRC_FRONT_VOCTREE_H
#define NDRC_FRONT_VOCTREE_H

#include <stddef.h>

#include "arena.h"
#include "diag.h"
#include "symbols.h"
#include "vec.h"

/* PORT: TVocType (UVocabularyTree.pas:9), member for member, same
   order. VOC_CONJUGATION and VOC_PRONOUN are declared but never
   referenced by any unit this port covers (analysis 26.1); ported
   anyway for fidelity to the Pascal enum. */
typedef enum VocType {
    VOC_VERB,
    VOC_ADVERB,
    VOC_NOUN,
    VOC_ADJECT,
    VOC_PREPOSITION,
    VOC_CONJUGATION,
    VOC_PRONOUN,
    VOC_ANY
} VocType;

/* PORT: TVocabularyTree's data fields (UVocabularyTree.pas:13-19),
   Left/Right omitted - those are this container's own internal tree
   shape, not part of the read surface a caller needs. `voc_word` is
   the CANONICALISED (post-FixSpanishChars) stored text. */
typedef struct VocEntry {
    const char *voc_word;
    long value;
    VocType voc_type;
} VocEntry;

VEC_DECLARE(VocEntry, VocEntry *)

typedef struct VocTree VocTree;

VocTree *voctree_new(Arena *a);

/* PORT: AddVocabulary (UVocabularyTree.pas:75-79) + its recursive
   AddVocabularyInternal (54-73). Canonicalises `word` internally - do
   not pre-fold. Equality ignores `type`: a second insert of the same
   text under a DIFFERENT type is rejected like a true duplicate (26.1:
   one type per distinct spelling).

   On a genuinely NEW word, insertion has a side effect BEFORE
   returning success: `symbols_add(symbols, a, d, "_VOC_<canonical>",
   value)`. If THAT fails (the symbol already exists some other way),
   this call also returns 0 - defect 19.52: a false return conflates
   "word text already in the tree" with "new word, but its
   auto-generated symbol collided". Returns 1 on success. */
int voctree_add(VocTree *t, Arena *a, Diag *d, SymbolList *symbols,
                 const char *word, long value, VocType type);

/* PORT: GetVocabulary (91-95) + GetVocabularyInternal (82-89).
   Canonicalises `word` internally (do not pre-fold), then an ordinary
   BST search; VOC_ANY matches any stored type, otherwise the stored
   type must match exactly. Returns 1 and fills *out on a hit, 0 on a
   miss (*out left untouched). */
int voctree_lookup(const VocTree *t, Arena *a, const char *word,
                    VocType type, VocEntry *out);

/* PORT: GetVocabularyByNumber (97-109). NOT a BST search by value -
   the tree is keyed by text - so this is an unconditional walk of
   BOTH children, preferring the LEFT subtree's match when both exist
   (recurse left first, then right; left wins on a tie). Used by the
   JSON export (Task 8) to reconstruct the human-readable `Entry`
   string (analysis 16.6); exposed here since it is part of this
   unit's whole surface, not because Task 5 itself calls it. Returns 1
   and fills *out on a hit, 0 on a miss. */
int voctree_lookup_by_number(const VocTree *t, long value, VocType type,
                              VocEntry *out);

size_t voctree_count(const VocTree *t);

/* In-order (Left, self, Right) walk by canonical key ascending -
   exactly the order getVocabularyJSON's sorted emission needs (26.1's
   closing note). One O(n) pass, unlike a naive per-index voctree_at
   (O(n^2) on this unbalanced tree), flattened once for JSON export.
   Entries point at the tree's own arena-allocated storage (stable),
   not copies. */
Vec_VocEntry *voctree_inorder(const VocTree *t, Arena *a);

#endif /* NDRC_FRONT_VOCTREE_H */
