/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEMPEST_FEX_EXPORTS_H
#define TEMPEST_FEX_EXPORTS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t *gpr;
    uint64_t (*xmm)[2];
    uintptr_t rsp;
} TempestFexFrame;
typedef struct {
    const char *name;
    void *function;
    void (*invoke)(TempestFexFrame *);
    size_t stack_bytes;
} TempestFexExport;
#ifdef __cplusplus
}
#endif
#endif
