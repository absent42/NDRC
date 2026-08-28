/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/phpround.h - PHP's round() as the reference actually rounds.
   Copyright (C) 2026 Dan Gibson. */
#ifndef NDRC_BACK_PHPROUND_H
#define NDRC_BACK_PHPROUND_H

/* PORT: PHP's round($value) with the default precision of 0 digits
   (ext/standard/math.c, _php_math_round). NOT C's round(): PHP
   PRE-ROUNDS to the decimal precision floating point can still be
   trusted at, so a product that lands one ULP BELOW a half-way point
   rounds UP in PHP and DOWN in C.

   This is not a curiosity; it decides real DDB bytes. drb.php:873's
   duration adjustment on a base length of 230 (MSX, MSX2, HTML) turns
   `PAUSE 50` into 50 * 1.15, which is 57.49999999999999289457 as a
   double - PHP writes 58, C's round() writes 57. Six of the 256
   reachable Param1 values diverge on that base length (50, 90, 110,
   170, 190, 210); every other base length in targets.c (80, 100, 120,
   195, 200, 300) diverges on none.

   Measured, not assumed: this reproduction was validated against the
   very php.exe the reference flow runs over 46279 values - every
   reachable base-length x Param1 product, a +/-4 ULP sweep around every
   half-integer from -4.5 to 400.5, and 40000 pseudo-random doubles in
   [-500, 500] - with zero mismatches. See tests/test_php_round.c for the
   rows pinned from that run.

   VERSION FACT: measured on PHP 7.4.30, the interpreter the oracle's own
   toolchain ships. PHP 8.4 changed round()'s edge-case handling; if the
   reference flow is ever moved to it, this needs re-measuring. */
double php_round(double value);

#endif /* NDRC_BACK_PHPROUND_H */
