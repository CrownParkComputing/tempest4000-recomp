// SPDX-License-Identifier: GPL-2.0-or-later
// tempest: host timing and spin hints for both desktop x86 and Android ARM64.
#pragma once
#include "common/types.h"
namespace Common {
inline u64 ReadHostCounter() {
#if defined(__x86_64__)
    u32 low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (u64(high) << 32) | low;
#elif defined(__aarch64__)
    u64 value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
#error Unsupported host CPU
#endif
}
inline void PauseHostThread() {
#if defined(__x86_64__)
    asm volatile("pause");
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
}
