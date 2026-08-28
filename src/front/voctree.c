/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/front/voctree.c - Copyright (C) 2026 Dan Gibson. */
#include "voctree.h"

#include <string.h>

#include "str.h"

/* PORT: FixSpanishChars (UVocabularyTree.pas:39-50). Eight byte-for-
   byte replacements, each mapping an UPPER-case accented Latin-1 byte
   DOWN to lower-case. Applied AFTER str_upper_latin1 in
   voc_canonicalize below, matching AddVocabulary/GetVocabulary's
   FixSpanishChars(AnsiUpperCase(word)) order. No replacement's target
   byte is ever another's source byte, so table order is safe. */
static const struct { unsigned char from, to; } FIX_SPANISH[8] = {
    {0xC1, 0xE1}, /* A-acute -> a-acute */
    {0xC9, 0xE9}, /* E-acute -> e-acute */
    {0xCD, 0xED}, /* I-acute -> i-acute */
    {0xD3, 0xF3}, /* O-acute -> o-acute */
    {0xDA, 0xFA}, /* U-acute -> u-acute */
    {0xDC, 0xFC}, /* U-diaeresis -> u-diaeresis */
    {0xD1, 0xF1}, /* N-tilde -> n-tilde */
    {0xC7, 0xE7}, /* C-cedilla -> c-cedilla */
};

static char *fix_spanish_chars(Arena *a, const char *s)
{
    char *out = arena_strdup(a, s);
    unsigned char *p;
    for (p = (unsigned char *)out; *p; p++) {
        int i;
        for (i = 0; i < 8; i++) {
            if (*p == FIX_SPANISH[i].from) {
                *p = FIX_SPANISH[i].to;
                break;
            }
        }
    }
    return out;
}

/* PORT: `FixSpanishChars(AnsiUpperCase(word))`, the exact canonical
   form both AddVocabulary (75-79) and GetVocabulary (91-95) compute
   before touching the tree - one helper, since every entry point
   needs the identical transform. */
static char *voc_canonicalize(Arena *a, const char *word)
{
    return fix_spanish_chars(a, str_upper_latin1(a, word));
}

typedef struct VocNode {
    VocEntry entry;
    struct VocNode *left, *right;
} VocNode;

struct VocTree {
    VocNode *root;
};

VocTree *voctree_new(Arena *a)
{
    VocTree *t = arena_alloc(a, sizeof(*t));
    t->root = NULL;
    return t;
}

/* PORT: AddVocabularyInternal (UVocabularyTree.pas:54-73). `word` is
   ALREADY canonicalised by the caller (voctree_add). */
static int voctree_add_internal(VocNode **node, Arena *a, Diag *d,
                                 SymbolList *symbols, const char *word,
                                 long value, VocType type)
{
    int cmp;
    if (*node == NULL) {
        char *voc_symbol;
        VocNode *n = arena_alloc(a, sizeof(*n));
        n->entry.voc_word = word;
        n->entry.value = value;
        n->entry.voc_type = type;
        n->left = NULL;
        n->right = NULL;
        *node = n;

        /* PORT: the `_VOC_<word>` AddSymbol side effect
           (UVocabularyTree.pas:70), gated on the NEW node only, before
           this insert can be reported successful. */
        voc_symbol = arena_alloc(a, strlen(word) + 6);
        strcpy(voc_symbol, "_VOC_");
        strcat(voc_symbol, word);
        return symbols_add(symbols, a, d, voc_symbol, value);
    }

    cmp = strcmp(word, (*node)->entry.voc_word);
    if (cmp > 0) {
        return voctree_add_internal(&(*node)->right, a, d, symbols, word,
                                     value, type);
    }
    if (cmp < 0) {
        return voctree_add_internal(&(*node)->left, a, d, symbols, word,
                                     value, type);
    }
    return 0; /* equal: reject, ignoring `type` per 26.1 */
}

int voctree_add(VocTree *t, Arena *a, Diag *d, SymbolList *symbols,
                 const char *word, long value, VocType type)
{
    char *canon = voc_canonicalize(a, word);
    return voctree_add_internal(&t->root, a, d, symbols, canon, value,
                                 type);
}

/* PORT: GetVocabularyInternal (82-89). */
static const VocNode *voctree_lookup_internal(const VocNode *node,
                                               const char *word,
                                               VocType type)
{
    int cmp;
    if (node == NULL) return NULL;
    cmp = strcmp(node->entry.voc_word, word);
    if (cmp == 0 && (type == VOC_ANY || node->entry.voc_type == type)) {
        return node;
    }
    if (cmp > 0) return voctree_lookup_internal(node->left, word, type);
    return voctree_lookup_internal(node->right, word, type);
}

int voctree_lookup(const VocTree *t, Arena *a, const char *word,
                    VocType type, VocEntry *out)
{
    char *canon = voc_canonicalize(a, word);
    const VocNode *n = voctree_lookup_internal(t->root, canon, type);
    if (n == NULL) return 0;
    *out = n->entry;
    return 1;
}

/* PORT: GetVocabularyByNumber (97-109). Pascal computes PTR1/PTR2 both
   and prefers PTR1; returning on the first (left) hit is
   observationally identical - no side effects. */
static const VocNode *voctree_lookup_by_number_internal(const VocNode *node,
                                                          long value,
                                                          VocType type)
{
    const VocNode *left, *right;
    if (node == NULL) return NULL;
    if (node->entry.value == value &&
        (type == VOC_ANY || node->entry.voc_type == type)) {
        return node;
    }
    left = voctree_lookup_by_number_internal(node->left, value, type);
    if (left != NULL) return left;
    right = voctree_lookup_by_number_internal(node->right, value, type);
    return right;
}

int voctree_lookup_by_number(const VocTree *t, long value, VocType type,
                              VocEntry *out)
{
    const VocNode *n = voctree_lookup_by_number_internal(t->root, value,
                                                           type);
    if (n == NULL) return 0;
    *out = n->entry;
    return 1;
}

static size_t voctree_count_internal(const VocNode *node)
{
    if (node == NULL) return 0;
    return 1 + voctree_count_internal(node->left) +
           voctree_count_internal(node->right);
}

size_t voctree_count(const VocTree *t)
{
    return voctree_count_internal(t->root);
}

static void voctree_inorder_walk(const VocNode *node, Vec_VocEntry *out)
{
    if (node == NULL) return;
    voctree_inorder_walk(node->left, out);
    vec_push_VocEntry(out, (VocEntry *)&node->entry);
    voctree_inorder_walk(node->right, out);
}

Vec_VocEntry *voctree_inorder(const VocTree *t, Arena *a)
{
    Vec_VocEntry *out = vec_new_VocEntry(a);
    voctree_inorder_walk(t->root, out);
    return out;
}
