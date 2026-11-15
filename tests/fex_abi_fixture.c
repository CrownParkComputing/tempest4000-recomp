/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Side effects and register classes used to exercise generated x86 -> ARM adapters. */
#include "runtime.h"
static ABI int32_t fixture_negative(void) { return -37; }
static ABI void fixture_store(uint64_t *out, uint64_t value) { *out = value; }
static ABI double fixture_mixed(uint64_t a, double x, uint64_t b, uint64_t c,
                                uint64_t d, uint64_t e, uint64_t f, uint64_t g, double y) {
    return (double)(a + b + c + d + e + f + g) + x * y;
}
