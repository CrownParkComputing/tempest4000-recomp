/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include <stdio.h>
#include <unistd.h>
/* Tempest does not import fibers; keep an explicit unresolved boundary. */
uintptr_t runtime_fiber_resolve(const char *name) { (void)name; return 0; }
void runtime_restart(void) {
    fputs("STOP: Android activity restart is not implemented\n",stderr);
    fflush(NULL); _exit(21);
}
static ABI void process_finalizer(void) { puts("Runtime: process finalizer callback reached"); }
uintptr_t tempest_boot_finalizer(void) { return (uintptr_t)process_finalizer; }
