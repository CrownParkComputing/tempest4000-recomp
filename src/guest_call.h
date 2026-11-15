/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest callbacks run directly on x86-64 and through the CPU backend on ARM64. */
#ifndef TEMPEST_GUEST_CALL_H
#define TEMPEST_GUEST_CALL_H
#include <stddef.h>
#include <stdint.h>
#ifdef TEMPEST_FEX
#ifdef __cplusplus
extern "C" {
#endif
uintptr_t runtime_fex_call(uintptr_t entry, const uintptr_t *arguments, size_t count);
uintptr_t runtime_thread_tcb(void);
#ifdef __cplusplus
}
#endif
#define GUEST_CALL0(type, entry) ((void)sizeof(type), runtime_fex_call((uintptr_t)(entry), NULL, 0))
#define GUEST_CALL1(type, entry, a) ((void)sizeof(type), runtime_fex_call((uintptr_t)(entry), (const uintptr_t[]){(uintptr_t)(a)}, 1))
#define GUEST_CALL3(type, entry, a, b, c) ((void)sizeof(type), runtime_fex_call((uintptr_t)(entry), (const uintptr_t[]){(uintptr_t)(a), (uintptr_t)(b), (uintptr_t)(c)}, 3))
#else
#define GUEST_CALL0(type, entry) ((type)(entry))()
#define GUEST_CALL1(type, entry, a) ((type)(entry))(a)
#define GUEST_CALL3(type, entry, a, b, c) ((type)(entry))(a, b, c)
#endif
#endif
