/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_obj.c - object table emitters.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:716-765 generateObjectNames, generateObjectInitially,
   generateObjectWeightAndAttr, generateObjectExtraAttr. */
#include "emit.h"

void emit_object_names(Str *out, long *addr, const Adventure *adv)
{
    size_t i, n = vec_len_ObjectData(adv->object_data);

    for (i = 0; i < n; i++) {
        const ObjectData *o = vec_at_ObjectData(adv->object_data, i);
        str_push_u8(out, (unsigned)o->Noun);
        str_push_u8(out, (unsigned)o->Adjective);
        *addr += 2;
    }
}

void emit_object_initially(Str *out, long *addr, const Adventure *adv)
{
    size_t i, n = vec_len_ObjectData(adv->object_data);

    for (i = 0; i < n; i++) {
        const ObjectData *o = vec_at_ObjectData(adv->object_data, i);
        str_push_u8(out, (unsigned)o->InitialyAt);
        (*addr)++;
    }
    str_push_u8(out, 0xFFu);
    (*addr)++;
}

/* PORT NOTE (analysis S12.1): drb.php:746-747 reads
   $adventure->objects[$locno]->Text - OTX, indexed by the container's
   own Value, NOT the locations table the surrounding checks reason
   about; reproduced verbatim, not corrected. Two PHP null behaviours
   the guards below depend on: locno outside objects yields
   NULL->Text->"" (substituted here for text); locno negative still
   passes count()<=locno (never negative), so the undefined-index read
   yields NULL, NULL != '' is false, and neither warning fires -
   matched by falling through without printing. */
void emit_object_weight_attr(Str *out, long *addr, Diag *d, const Adventure *adv)
{
    size_t i, n = vec_len_ObjectData(adv->object_data);
    long locations_n = (long)vec_len_Message(adv->locations);
    size_t objects_n = vec_len_Message(adv->objects);

    for (i = 0; i < n; i++) {
        const ObjectData *o = vec_at_ObjectData(adv->object_data, i);
        long b = o->Weight & 0x3F;

        if (o->Container) {
            long locno = o->Value;
            const char *text = "";

            b |= 0x40;
            if (locno >= 0 && (size_t)locno < objects_n) {
                const Message *otext = vec_at_Message(adv->objects, (size_t)locno);
                text = str_cstr(otext->Text);
            }
            if (locations_n <= locno) {
                diag_note(d,
                    "Warning: object #%ld (%s) is a container. You are "
                    "supposed to reserve location #%ld to hold the objects "
                    "in the container, but location #%ld does not exist.",
                    locno, text, locno, locno);
            } else if (locno >= 0) {
                const Message *ltext = vec_at_Message(adv->locations, (size_t)locno);
                if (str_len(ltext->Text) != 0) {
                    diag_note(d,
                        "Warning: object #%ld (%s) is a container. You are "
                        "supposed to reserve location #%ld to hold the "
                        "objects in the container, but location #%ld has a "
                        "description.",
                        locno, text, locno, locno);
                }
            }
            /* locno < 0: neither warning fires, per the PORT NOTE above. */
        }
        if (o->Wearable) b |= 0x80;
        str_push_u8(out, (unsigned)b);
        (*addr)++;
    }
}

void emit_object_extra_attr(Str *out, long *addr, const Target *t, const Adventure *adv)
{
    size_t i, n = vec_len_ObjectData(adv->object_data);

    for (i = 0; i < n; i++) {
        const ObjectData *o = vec_at_ObjectData(adv->object_data, i);
        str_push_u16(out, (unsigned)o->Flags, t->big_endian);
        *addr += 2;
    }
}
