/* SPDX-License-Identifier: GPL-3.0-or-later */
/* src/back/phpround.c - PHP round() reproduction. See phpround.h for the
   measurement this was validated against.
   Copyright (C) 2026 Dan Gibson. */
#include <math.h>

#include "back/phpround.h"

/* PORT: php_round_helper (math.c) under the default PHP_ROUND_HALF_UP -
   half away from zero, which is what C's round() does too. The
   difference between the two languages is NOT here; it is the
   pre-rounding in php_round below. */
static double round_half_up(double value)
{
    return value >= 0.0 ? floor(value + 0.5) : ceil(value - 0.5);
}

/* PORT: php_intpow10 (math.c) - a lookup table, not pow(), so the powers
   are the exact doubles PHP scales by. PHP asserts 0..22; the callers
   below cannot exceed that range (see php_round's own clamp), and the
   clamp here keeps the indexing safe regardless. */
static double intpow10(int power)
{
    static const double p[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
        1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
        1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    if (power < 0) power = 0;
    if (power > 22) power = 22;
    return p[power];
}

/* PORT: _php_math_round (math.c) with places == 0, which is every call
   the ported drb.php makes. The whole point is the pre-round: PHP
   computes precision_places = 14 - floor(log10(|value|)), and when that
   sits strictly between the requested places (0) and places + 15, it
   first rounds the value at THAT many decimal digits before rounding to
   an integer. A value one ULP below a half-way point pre-rounds up ONTO
   the half-way point and then rounds away from zero, where plain C
   round() would take it down.

   With places == 0 the reference's f1 scaling (multiply then divide by
   10^0) is the identity, so it is folded out rather than written as a
   pair of no-ops. */
double php_round(double value)
{
    double tmp;
    int precision_places;

    if (!isfinite(value) || value == 0.0) return value;

    precision_places = (int)(14.0 - floor(log10(fabs(value))));

    if (precision_places > 0 && precision_places < 15) {
        double f2 = intpow10(precision_places);
        tmp = round_half_up(value * f2) / f2;
    } else {
        /* No pre-round: precision_places has fallen outside the window,
           so the value is either large enough that 14 significant digits
           reach past the decimal point already, or small enough that
           pre-rounding could only flatten it to zero. PHP rounds it
           as-is - and returns it entirely untouched once it is beyond
           what the format can represent to unit precision. */
        if (fabs(value) >= 1e15) return value;
        tmp = value;
    }

    return round_half_up(tmp);
}
