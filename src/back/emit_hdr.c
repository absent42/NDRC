/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_hdr.c - DDB header emitter and offset patch-up.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php's header write (drb.php:1834-1874) and the header patch
   pass (drb.php:2039-2073). */
#include "emit_hdr.h"

void emit_header(Str *out, long *addr, const Target *t, const Adventure *adv, int lang_bit)
{
    unsigned version = adv->v3code ? 3u : 2u;
    unsigned machine_lang = ((unsigned)t->machine_id << 4) | (lang_bit ? 1u : 0u);
    int i;

    str_push_u8(out, version);
    str_push_u8(out, machine_lang);
    str_push_u8(out, (unsigned)t->submachine_id);

    str_push_u8(out, (unsigned)vec_len_ObjectData(adv->object_data));
    str_push_u8(out, (unsigned)vec_len_Message(adv->locations));
    str_push_u8(out, (unsigned)vec_len_Message(adv->messages));
    str_push_u8(out, (unsigned)vec_len_Message(adv->sysmess));
    str_push_u8(out, (unsigned)vec_len_Process(adv->processes));

    /* drb.php:1869 writeBlock(...,26): the rest of the header, filled
       with zeros until the patch pass below knows the real offsets. */
    for (i = 0; i < 26; i++) str_push_u8(out, 0);
    *addr += 34;

    /* drb.php:1872-1873: extvec is all zero here (drb.php:1783-1784),
       and nothing has run yet that could have changed it. */
    for (i = 0; i < 13; i++) str_push_u16(out, (unsigned)adv->extvec[i], t->big_endian);
    *addr += 26;
}

void emit_header_patch(Str *out, const Target *t, const Adventure *adv,
                        const long offsets[NDRC_HEADER_PATCH_WORDS])
{
    size_t i;

    for (i = 0; i < NDRC_HEADER_PATCH_WORDS; i++) {
        str_set_u16(out, 8 + 2 * i, (unsigned)offsets[i], t->big_endian);
    }
    for (i = 0; i < 13; i++) {
        str_set_u16(out, 34 + 2 * i, (unsigned)adv->extvec[i], t->big_endian);
    }
}
