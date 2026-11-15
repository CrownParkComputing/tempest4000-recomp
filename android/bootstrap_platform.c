/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The boot check has no renderer or SDL device services. Their imports stay unresolved. */
#include "runtime.h"
#include "gpu/tempest_gpu.h"
#include <stdio.h>
#include <unistd.h>
uintptr_t runtime_audio_resolve(const char *name) { (void)name; return 0; }
void runtime_audio_report(void) {}
uintptr_t runtime_pad_resolve(const char *name) { (void)name; return 0; }
void runtime_pad_report(void) {}
void runtime_pad_title_movie(int playing) { (void)playing; }
uintptr_t runtime_fiber_resolve(const char *name) { (void)name; return 0; }
uintptr_t tempest_gpu_resolve(const char *name) { (void)name; return 0; }
int tempest_gpu_handle_fault(void *context, void *address) { (void)context; (void)address; return 0; }
int tempest_skip_intro(void) { return 1; }
int tempest_frame_rate(void) { return 60; }
void tempest_set_frame_rate(int rate) { (void)rate; }
int tempest_gpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int tempest_gpu_text_input_poll(char *out, uint64_t size) { (void)out; (void)size; return 2; }
int tempest_gpu_choice_begin(const char *title, const char *const *items, const char *const *images,
                             int count, int focus) {
    (void)title; (void)items; (void)images; (void)count; (void)focus; return 0;
}
int tempest_gpu_choice_poll(void) { return -2; }
int tempest_gpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                               const unsigned char *png, uint64_t size, const char *caption, const char *unavailable) {
    (void)title; (void)items; (void)count; (void)focus; (void)png; (void)size; (void)caption; (void)unavailable;
    return 0;
}
void tempest_gpu_notice(const char *text, int ms) { (void)ms; printf("Bootstrap notice: %s\n", text); }
void tempest_gpu_toast_trophy(int id, const char *title, const char *detail, const unsigned char *png,
                             uint64_t size) { (void)id; (void)title; (void)detail; (void)png; (void)size; }
void tempest_gpu_common_dialog_initialized(void) {}
void runtime_restart(void) { fputs("STOP: restart requested in Android bootstrap\n", stderr); _exit(21); }
static ABI void process_finalizer(void) { puts("Runtime: process finalizer callback reached"); }
uintptr_t tempest_boot_finalizer(void) { return (uintptr_t)process_finalizer; }
