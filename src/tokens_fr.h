/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/tokens_fr.h - builtin FR compression token table.
   Copyright (C) 2026 Dan Gibson.

   Ported from drb.php:139 ($compressionJSON_FR). The string below is
   COPIED VERBATIM, byte for byte, from that PHP source line - do not
   hand-edit it; regenerate from drb.php:139 instead. 129 hex tokens,
   JSON object {"compression": "advanced", "tokens": [...]}. Each hex
   pair decodes (via a ported hex2str, drb.php:300-307) to one raw
   output byte.
   PORT NOTE: unlike the other four languages, token[0] here is "7f",
   not "00" - and the table's final 34 entries are all "7f" too. Both
   are transcribed verbatim from drb.php:139, not a transcription error;
   this port makes no attempt to "fix" DRB's FR table. */
#ifndef NDRC_TOKENS_FR_H
#define NDRC_TOKENS_FR_H

#define NDRC_COMPRESSION_JSON_FR \
    "{\"compression\": \"advanced\", \"tokens\": [\"7f\", \"4a65206e6520\", \"706f72746520\", \"20646520\", \"4a6520\", \"782070617320\", \"666169726520\", \"746520\", \"2064616e7320\", \"205f2e0d\", \"4a27616920\", \"6e6520\", \"20706f757220\", \"6d61696e7465\", \"657220\", \"656e6c6576\", \"706575\", \"65722e0d\", \"61207269656e\", \"63656c612e\", \"0d51756520\", \"20706c65696e\", \"707579657a20\", \"20706173\", \"64166a0e100f\", \"652e0d\", \"6e616e74203f\", \"6c6520\", \"746f75636865\", \"73757220\", \"721665737361\", \"2e0d\", \"656e737569\", \"6c657a20\", \"6f7520\", \"65722e\", \"6d0e140f\", \"6e2761692072\", \"0d457420\", \"2074726f7020\", \"6c6965752e20\", \"496c206e2779\", \"6f6e74696e75\", \"657374\", \"7265\", \"6973717565\", \"6d6f69203a0d\", \"726f75\", \"6173736574\", \"6f737369\", \"69656e\", \"7320\", \"6669636869\", \"6572\", \"205f2e\", \"7572\", \"6f6d\", \"63656c61\", \"766f69\", \"73756973\", \"706f7274\", \"6e616e74\", \"0e160f74\", \"6169\", \"4c6520\", \"4020\", \"656374\", \"657a20\", \"452044\", \"7365\", \"617520\", \"0e140f\", \"6f6e\", \"202d20\", \"6575\", \"6f75\", \"6d70\", \"6574\", \"6170\", \"7061\", \"6e27\", \"616c\", \"6f69\", \"7573\", \"2e20\", \"7072\", \"2e2e\", \"6f72\", \"2064\", \"2063\", \"6369\", \"3a20\", \"6d65\", \"2056\", \"642e\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\", \"7f\"]}"

#endif /* NDRC_TOKENS_FR_H */
