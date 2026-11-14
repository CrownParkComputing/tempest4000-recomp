/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NGS2, the PS4's voice mixer (libSceNgs2). Tempest 4000 builds a system, one rack of voices in
 * it, and renders that system into its own buffers, which it then writes to libSceAudioOut.
 *
 * This keeps the systems, racks and voices the game creates and reads back, and renders silence
 * into the buffers handed to sceNgs2SystemRender: the game's audio clock advances and its voices
 * recycle, but no sound is produced yet. Mixing the voices is the next step. A voice reports no
 * state flags, so the game treats one as free rather than waiting for a sound that never ends.
 *
 * The structures follow shadPS4's core/libraries/ngs2/ngs2.h (GPL-2.0-or-later), as do the error
 * codes; the game's own allocator is not called, because its handlers are guest code and nothing
 * here needs the memory to come from the guest's pool. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define ERR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)
#define ERR_INVALID_OUT_SIZE ((int32_t)0x804A0054)
#define ERR_INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define ERR_INVALID_SYSTEM_HANDLE ((int32_t)0x804A0230)
#define ERR_INVALID_RACK_HANDLE ((int32_t)0x804A0261)
#define ERR_INVALID_VOICE_HANDLE ((int32_t)0x804A0300)
#define ERR_INVALID_VOICE_INDEX ((int32_t)0x804A0302)
#define ERR_OUT_OF_MEMORY ((int32_t)0x804A0004)

/* The PS4 hands a rack as many voices as its option asks for; without an option it is this many,
 * which is what the sampler rack defaults to. */
#define DEFAULT_VOICES 64
#define MAX_VOICES 512

typedef struct { void *host_buffer; uint64_t host_buffer_size; uintptr_t reserved[5]; uintptr_t user_data; } ContextBufferInfo;
typedef struct { void *buffer; uint64_t buffer_size; uint32_t waveform_type; uint32_t channels; } RenderBufferInfo;
typedef struct { uint32_t state_flags; } VoiceState;

enum { SYSTEM_MAGIC=0x4e475332, RACK_MAGIC=0x4e475233, VOICE_MAGIC=0x4e475234 };

typedef struct Rack Rack;
typedef struct { uint32_t magic, state; Rack *rack; uint32_t index; } Voice;
struct Rack { uint32_t magic; void *system; uint32_t voice_count; Voice voices[]; };
typedef struct { uint32_t magic; } System;

static size_t systems, racks, voices_handed, renders, silent_bytes;

/* sceNgs2SystemCreateWithAllocator(option, allocator, outHandle): the option names the grain size
 * and voice limits, none of which a silent renderer needs. */
static ABI int32_t system_create(const void *option, const void *allocator, void **out) {
    (void)option; (void)allocator;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    System *s = runtime_low_alloc(sizeof(*s));
    if (!s) return ERR_OUT_OF_MEMORY;
    s->magic = SYSTEM_MAGIC;
    *out = s; ++systems;
    return 0;
}
/* sceNgs2SystemDestroy(handle, outBufferInfo): the buffer the system was built in is reported
 * back so the game can free it; this one allocated none, so the report is empty. */
static ABI int32_t system_destroy(void *handle, ContextBufferInfo *out) {
    System *s = handle;
    if (!s || s->magic != SYSTEM_MAGIC) return ERR_INVALID_SYSTEM_HANDLE;
    s->magic = 0;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}
/* sceNgs2SystemRender(handle, buffers, count): fills each buffer with one grain of output.
 * Silence is all zeroes for every waveform type this game uses (float and signed PCM). */
static ABI int32_t system_render(void *handle, const RenderBufferInfo *buffers, uint32_t count) {
    System *s = handle;
    if (!s || s->magic != SYSTEM_MAGIC) return ERR_INVALID_SYSTEM_HANDLE;
    if (count && !buffers) return ERR_INVALID_BUFFER_ADDRESS;
    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i].buffer || !buffers[i].buffer_size) continue;
        memset(buffers[i].buffer, 0, (size_t)buffers[i].buffer_size);
        silent_bytes += (size_t)buffers[i].buffer_size;
    }
    ++renders;
    return 0;
}
/* sceNgs2RackCreateWithAllocator(system, rackId, option, allocator, outHandle): a rack is a bank
 * of voices of one kind. The option's second word is its voice count on every rack type. */
static ABI int32_t rack_create(void *system, uint32_t rack_id, const void *option,
                               const void *allocator, void **out) {
    (void)rack_id; (void)allocator;
    System *s = system;
    if (!s || s->magic != SYSTEM_MAGIC) return ERR_INVALID_SYSTEM_HANDLE;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    uint32_t count = DEFAULT_VOICES;
    if (option) {
        /* OrbisNgs2RackOption: size, name[16], flags, maxGrainSamples, numVoices, ... */
        uint32_t asked = *(const uint32_t *)((const unsigned char *)option + sizeof(size_t) + 16 + 8);
        if (asked && asked <= MAX_VOICES) count = asked;
    }
    Rack *r = runtime_low_alloc(sizeof(*r) + count * sizeof(Voice));
    if (!r) return ERR_OUT_OF_MEMORY;
    r->magic = RACK_MAGIC; r->system = s; r->voice_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        r->voices[i].magic = VOICE_MAGIC; r->voices[i].state = 0;
        r->voices[i].rack = r; r->voices[i].index = i;
    }
    *out = r; ++racks;
    return 0;
}
static ABI int32_t rack_destroy(void *handle, ContextBufferInfo *out) {
    Rack *r = handle;
    if (!r || r->magic != RACK_MAGIC) return ERR_INVALID_RACK_HANDLE;
    r->magic = 0;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}
/* sceNgs2RackGetVoiceHandle(rack, index, outHandle): voices are numbered inside their rack. */
static ABI int32_t rack_voice_handle(void *handle, uint32_t index, void **out) {
    Rack *r = handle;
    if (!r || r->magic != RACK_MAGIC) return ERR_INVALID_RACK_HANDLE;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (index >= r->voice_count) return ERR_INVALID_VOICE_INDEX;
    *out = &r->voices[index]; ++voices_handed;
    return 0;
}
/* sceNgs2VoiceControl(voice, paramList): a chain of parameter blocks (set a waveform, a pitch,
 * a matrix, play, stop). Nothing is mixed yet, so the chain is only validated. */
static ABI int32_t voice_control(void *handle, const void *param_list) {
    Voice *v = handle;
    if (!v || v->magic != VOICE_MAGIC) return ERR_INVALID_VOICE_HANDLE;
    (void)param_list;
    return 0;
}
/* sceNgs2VoiceGetState(voice, outState, size) and sceNgs2VoiceGetStateFlags(voice, outFlags):
 * no flags means the voice is neither in use nor playing, so the game is free to reuse it. */
static ABI int32_t voice_get_state(void *handle, VoiceState *out, uint64_t size) {
    Voice *v = handle;
    if (!v || v->magic != VOICE_MAGIC) return ERR_INVALID_VOICE_HANDLE;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (size < sizeof(*out)) return ERR_INVALID_OUT_SIZE;
    out->state_flags = v->state;
    return 0;
}
static ABI int32_t voice_get_state_flags(void *handle, uint32_t *out) {
    Voice *v = handle;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (!v || v->magic != VOICE_MAGIC) { *out = 0; return ERR_INVALID_VOICE_HANDLE; }
    *out = v->state;
    return 0;
}
/* sceNgs2GetWaveformFrameInfo(format, outFrameSize, outFrameSamples, outUnitsPerFrame,
 * outDelaySamples): how one frame of a waveform format is laid out. The game asks before it
 * sizes its own buffers, so the answer has to be consistent: linear PCM, one sample a unit. */
static ABI int32_t waveform_frame_info(const void *format, uint32_t *frame_size,
                                       uint32_t *frame_samples, uint32_t *units_per_frame,
                                       uint32_t *delay_samples) {
    if (!format) return ERR_INVALID_BUFFER_ADDRESS;
    /* OrbisNgs2WaveformFormat: waveformType, numChannels, sampleRate, configData, frameOffset, ... */
    uint32_t channels = ((const uint32_t *)format)[1];
    if (!channels) channels = 1;
    if (frame_size) *frame_size = 4 * channels;   /* 32-bit samples, one frame a sample */
    if (frame_samples) *frame_samples = 1;
    if (units_per_frame) *units_per_frame = 1;
    if (delay_samples) *delay_samples = 0;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNgs2SystemCreateWithAllocator",system_create}, {"sceNgs2SystemDestroy",system_destroy},
    {"sceNgs2SystemRender",system_render},
    {"sceNgs2RackCreateWithAllocator",rack_create}, {"sceNgs2RackDestroy",rack_destroy},
    {"sceNgs2RackGetVoiceHandle",rack_voice_handle},
    {"sceNgs2VoiceControl",voice_control},
    {"sceNgs2VoiceGetState",voice_get_state}, {"sceNgs2VoiceGetStateFlags",voice_get_state_flags},
    {"sceNgs2GetWaveformFrameInfo",waveform_frame_info},
};
uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_ngs2_report(void) {
    if (!systems && !racks) return;
    printf("Runtime: NGS2 systems=%zu, racks=%zu, voice handles=%zu, renders=%zu, silence=%zu bytes\n",
           systems, racks, voices_handed, renders, silent_bytes);
}
