/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/emit_con.c - connections table and lookup emitter.
   Copyright (C) 2026 Dan Gibson.

   PORT: drb.php:600-644 generateConnections. */
#include "emit.h"

#include "arena.h"

typedef struct {
    long direction;
    long to_loc;
} ConnPair;

VEC_DECLARE(ConnPair, ConnPair *)

long emit_connections(Str *out, long *addr, const Target *t, const Adventure *adv)
{
    size_t loc_n = vec_len_Message(adv->locations);
    size_t conn_n = vec_len_Connection(adv->connections);
    Vec_ConnPair **buckets = loc_n ? arena_alloc(str_arena(out), loc_n * sizeof(Vec_ConnPair *)) : NULL;
    long *offsets;
    long lookup_offset;
    size_t i, loc_id;

    /* PORT: drb.php:603-604 pre-creates one bucket per location. */
    for (loc_id = 0; loc_id < loc_n; loc_id++) buckets[loc_id] = vec_new_ConnPair(str_arena(out));

    /* PORT: drb.php:605-611 buckets by FromLoc, preserving arrival
       order. A connection whose FromLoc falls outside 0..loc_n-1 is
       bucketed by PHP too (array auto-vivification on
       $connectionsTable[$FromLoc][]=...), but the write loop below
       (drb.php:616, mirrored here) only ever visits locID 0..loc_n-1,
       so such a connection is silently never emitted either way -
       reproduced observably-identically by simply not bucketing it. */
    for (i = 0; i < conn_n; i++) {
        const Connection *c = vec_at_Connection(adv->connections, i);
        if (c->FromLoc >= 0 && (size_t)c->FromLoc < loc_n) {
            ConnPair *p = arena_alloc(str_arena(out), sizeof(*p));
            p->direction = c->Direction;
            p->to_loc = c->ToLoc;
            vec_push_ConnPair(buckets[c->FromLoc], p);
        }
    }

    offsets = loc_n ? arena_alloc(str_arena(out), loc_n * sizeof(long)) : NULL;
    for (loc_id = 0; loc_id < loc_n; loc_id++) {
        Vec_ConnPair *bucket = buckets[loc_id];
        size_t j;

        layout_pad(out, addr, t);
        offsets[loc_id] = *addr;
        for (j = 0; j < vec_len_ConnPair(bucket); j++) {
            const ConnPair *p = vec_at_ConnPair(bucket, j);
            str_push_u8(out, (unsigned)p->direction);
            str_push_u8(out, (unsigned)p->to_loc);
            *addr += 2;
        }
        str_push_u8(out, 0xFFu);   /* mark of end of connections */
        (*addr)++;
    }

    layout_pad(out, addr, t);
    lookup_offset = *addr;
    for (loc_id = 0; loc_id < loc_n; loc_id++) {
        str_push_u16(out, (unsigned)offsets[loc_id], t->big_endian);
        *addr += 2;
    }

    /* PORT NOTE (analysis S13): drb.php:641's trailing
       addPaddingIfRequired call happens AFTER the lookup table and
       BEFORE return - unlike every other table, whose next-odd-address
       alignment is left for the FOLLOWING table's own leading pad call.
       The main driver relies on this trailing pad having already run
       before it reads the post-call address to size the connections
       block, so it is ported here rather than left to a caller-side
       call. */
    layout_pad(out, addr, t);

    return lookup_offset;
}
