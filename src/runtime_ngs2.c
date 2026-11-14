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
#include <stdlib.h>
#include "libatrac9.h"

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
/* One ATRAC9 superframe is at most 1024 samples a channel (4 frames of 256). */
#define ATRAC9_MAX_SUPERFRAME 1024

typedef struct { void *host_buffer; uint64_t host_buffer_size; uintptr_t reserved[5]; uintptr_t user_data; } ContextBufferInfo;
typedef struct { void *buffer; uint64_t buffer_size; uint32_t waveform_type; uint32_t channels; } RenderBufferInfo;
typedef struct { uint32_t state_flags; } VoiceState;
/* A voice's parameters arrive as a chain: each block says its size and the byte offset of the
 * next one, so the whole chain can be walked without knowing any particular block. */
typedef struct { uint16_t size; int16_t next; uint32_t id; } VoiceParamHeader;
/* The format of a voice's sample data, and one block of it. Both follow shadPS4's ngs2.h. */
typedef struct { uint32_t type, channels, sample_rate, config, frame_offset, frame_margin; } WaveformFormat;
typedef struct { uint32_t data_offset, data_size, repeats, skip_samples, samples, reserved; uintptr_t user_data; } WaveformBlock;
/* Sampler voice parameters (ngs2_sampler.h): setting a voice up names its format, and the
 * waveform-blocks parameter hands it the sample data itself. */
#define PARAM_SAMPLER_SETUP 0x40010000u
#define PARAM_SAMPLER_WAVEFORM_BLOCKS 0x40010001u
/* Waveform types: the PCM family the render buffers use, and 0x40, which the game asks about
 * for its music. */
#define WAVEFORM_PCM_I16_LE 0x12u
#define WAVEFORM_PCM_I16_BE 0x13u
#define WAVEFORM_PCM_F32_LE 0x18u
#define WAVEFORM_PCM_F32_BE 0x19u
#define WAVEFORM_ATRAC9 0x40u
/* The one voice state flag the engine reads back: a voice that has been given parameters is in
 * use. Reporting nothing told the caller the voice was never allocated. */
#define VOICE_STATE_IN_USE 1u
#define VOICE_STATE_PLAYING 2u
#define ERR_INVALID_WAVEFORM_TYPE ((int32_t)0x804A0402)

enum { SYSTEM_MAGIC=0x4e475332, RACK_MAGIC=0x4e475233, VOICE_MAGIC=0x4e475234 };

typedef struct Rack Rack;
typedef struct {
    uint32_t magic, state;
    Rack *rack;
    uint32_t index;
    WaveformFormat format;     /* from PARAM_SAMPLER_SETUP */
    const unsigned char *data; /* from PARAM_SAMPLER_WAVEFORM_BLOCKS */
    uint32_t block_count;
    const WaveformBlock *blocks;
    /* Decoding state: one ATRAC9 superframe is decoded at a time and handed out a grain at a
     * time, because a grain is shorter than a superframe. */
    void *decoder;
    uint32_t block;            /* which block is playing */
    uint32_t read;             /* bytes consumed from it */
    float pcm[ATRAC9_MAX_SUPERFRAME * 2];
    uint32_t pcm_samples, pcm_taken;
    uint32_t decode_channels;
} Voice;
struct Rack { uint32_t magic; void *system; struct Rack *next; uint32_t voice_count; Voice voices[]; };
/* A system keeps its racks so rendering can walk every voice it owns. */
typedef struct { uint32_t magic; Rack *racks; } System;

static size_t systems, racks, voices_handed, renders, rendered_bytes, decoded_frames, mixed_voices;
/* RESOGUN_NGS2_TRACE=1 logs every call and what it answered: Scream's own error messages name
 * only its source line, so the call that upset it has to be found from this side. */
static int tracing(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("RESOGUN_NGS2_TRACE"); on = v ? atoi(v) : 0; }
    return on;
}
#define TRACE(...) do { if (tracing()) fprintf(stderr,"NGS2: " __VA_ARGS__); } while (0)

/* sceNgs2SystemCreateWithAllocator(option, allocator, outHandle): the option names the grain size
 * and voice limits, none of which a silent renderer needs. */
static ABI int32_t system_create(const void *option, const void *allocator, void **out) {
    (void)option; (void)allocator;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    System *s = runtime_low_alloc(sizeof(*s));
    if (!s) return ERR_OUT_OF_MEMORY;
    s->magic = SYSTEM_MAGIC; s->racks = NULL;
    *out = s; ++systems;
    TRACE("SystemCreateWithAllocator -> %p\n", (void *)s);
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
/* The next `want` samples a channel of a voice, decoded as needed, into `out` interleaved by
 * the voice's own channel count. Returns how many samples a channel were produced. */
static uint32_t voice_samples(Voice *v, float *out, uint32_t want) {
    if (!v->decoder || !v->data || !v->blocks || v->block >= v->block_count) return 0;
    uint32_t done = 0;
    while (done < want) {
        if (v->pcm_taken < v->pcm_samples) {
            uint32_t take = v->pcm_samples - v->pcm_taken;
            if (take > want - done) take = want - done;
            memcpy(out + (size_t)done * v->decode_channels,
                   v->pcm + (size_t)v->pcm_taken * v->decode_channels,
                   (size_t)take * v->decode_channels * sizeof(float));
            v->pcm_taken += take; done += take;
            continue;
        }
        /* Refill from the block that is playing; move on when it runs out. */
        const WaveformBlock *b = &v->blocks[v->block];
        if (v->read >= b->data_size) {
            if (++v->block >= v->block_count) break;
            v->read = 0;
            continue;
        }
        const unsigned char *at = v->data + b->data_offset + v->read;
        int used = 0;
        if (Atrac9DecodeF32(v->decoder, at, (int)(b->data_size - v->read), v->pcm, &used, 0) || used <= 0)
            break;
        v->read += (uint32_t)used;
        v->pcm_samples = ATRAC9_MAX_SUPERFRAME;   /* one superframe; the decoder fills what it has */
        v->pcm_taken = 0;
        decoded_frames++;
    }
    return done;
}

/* Adds a voice's samples into an interleaved output buffer. Channels beyond the voice's own are
 * left alone, so a stereo voice lands on the first two of a surround buffer. */
static void mix_into(const float *source, uint32_t source_channels, uint32_t samples,
                     void *buffer, uint32_t type, uint32_t channels) {
    for (uint32_t i = 0; i < samples; ++i)
        for (uint32_t c = 0; c < channels && c < source_channels; ++c) {
            float value = source[(size_t)i * source_channels + c];
            size_t at = (size_t)i * channels + c;
            if (type == WAVEFORM_PCM_F32_LE || type == WAVEFORM_PCM_F32_BE) {
                ((float *)buffer)[at] += value;
            } else {
                int32_t sum = ((int16_t *)buffer)[at] + (int32_t)(value * 32767.0f);
                ((int16_t *)buffer)[at] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
            }
        }
}

static ABI int32_t system_render(void *handle, const RenderBufferInfo *buffers, uint32_t count) {
    System *s = handle;
    if (!s || s->magic != SYSTEM_MAGIC) return ERR_INVALID_SYSTEM_HANDLE;
    if (count && !buffers) return ERR_INVALID_BUFFER_ADDRESS;
    static float scratch[ATRAC9_MAX_SUPERFRAME * 2];
    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i].buffer || !buffers[i].buffer_size) continue;
        memset(buffers[i].buffer, 0, (size_t)buffers[i].buffer_size);
        uint32_t channels = buffers[i].channels ? buffers[i].channels : 2;
        uint32_t bytes = (buffers[i].waveform_type == WAVEFORM_PCM_I16_LE ||
                          buffers[i].waveform_type == WAVEFORM_PCM_I16_BE) ? 2 : 4;
        uint32_t grain = (uint32_t)(buffers[i].buffer_size / (bytes * channels));
        if (grain > ATRAC9_MAX_SUPERFRAME) grain = ATRAC9_MAX_SUPERFRAME;
        /* Every playing voice of every rack of this system is added into the buffer. */
        for (Rack *r = s->racks; r; r = r->next)
            for (uint32_t n = 0; n < r->voice_count; ++n) {
                Voice *v = &r->voices[n];
                if (!(v->state & VOICE_STATE_PLAYING)) continue;
                uint32_t produced = voice_samples(v, scratch, grain);
                if (!produced) { v->state &= ~VOICE_STATE_PLAYING; continue; }
                mix_into(scratch, v->decode_channels, produced, buffers[i].buffer,
                         buffers[i].waveform_type, channels);
                mixed_voices++;
            }
        rendered_bytes += (size_t)buffers[i].buffer_size;
    }
    ++renders;
    if (renders <= 3 || !(renders % 500))
        TRACE("SystemRender(system=%p, buffers=%u) #%zu\n", handle, count, renders);
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
    r->next = s->racks; s->racks = r;
    for (uint32_t i = 0; i < count; ++i) {
        r->voices[i].magic = VOICE_MAGIC; r->voices[i].state = 0;
        r->voices[i].rack = r; r->voices[i].index = i;
    }
    *out = r; ++racks;
    TRACE("RackCreateWithAllocator(system=%p, rackId=%u, option=%p) -> %p, %u voices\n",
          system, rack_id, option, (void *)r, count);
    return 0;
}
static ABI int32_t rack_destroy(void *handle, ContextBufferInfo *out) {
    Rack *r = handle;
    if (!r || r->magic != RACK_MAGIC) return ERR_INVALID_RACK_HANDLE;
    System *owner = r->system;
    if (owner && owner->magic == SYSTEM_MAGIC)
        for (Rack **link = &owner->racks; *link; link = &(*link)->next)
            if (*link == r) { *link = r->next; break; }
    for (uint32_t i = 0; i < r->voice_count; ++i)
        if (r->voices[i].decoder) { Atrac9ReleaseHandle(r->voices[i].decoder); r->voices[i].decoder = NULL; }
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
    TRACE("RackGetVoiceHandle(rack=%p, index=%u) -> %p\n", handle, index, *out);
    return 0;
}
/* sceNgs2VoiceControl(voice, paramList): a chain of parameter blocks (set a waveform, a pitch,
 * a matrix, play, stop). Nothing is mixed yet, so the chain is only validated. */
static ABI int32_t voice_control(void *handle, const void *param_list) {
    Voice *v = handle;
    if (!v || v->magic != VOICE_MAGIC) return ERR_INVALID_VOICE_HANDLE;
    /* Any parameter at all means the engine has taken this voice; shadPS4's NGS2 reports the
     * same single flag. With no flag the caller reads the voice back as never allocated. */
    v->state |= VOICE_STATE_IN_USE;
    /* The chain: each block says its own size and the byte offset of the next, 0 ending it. */
    const unsigned char *at = param_list;
    for (unsigned guard = 0; at && guard < 64; ++guard) {
        const VoiceParamHeader *h = (const VoiceParamHeader *)at;
        if (!h->size) break;
        switch (h->id) {
        case PARAM_SAMPLER_SETUP: {
            const WaveformFormat *f = (const WaveformFormat *)(at + sizeof(*h));
            v->format = *f;
            TRACE("  setup voice=%p type=0x%x channels=%u rate=%u config=0x%x\n",
                  handle, f->type, f->channels, f->sample_rate, f->config);
            break;
        }
        case PARAM_SAMPLER_WAVEFORM_BLOCKS: {
            /* data, flags, numBlocks, aBlock */
            const unsigned char *body = at + sizeof(*h);
            const void *data; uint32_t flags, count; const WaveformBlock *blocks;
            memcpy(&data,body,sizeof(data));
            memcpy(&flags,body+8,4);
            memcpy(&count,body+12,4);
            memcpy(&blocks,body+16,sizeof(blocks));
            v->data = data; v->block_count = count; v->blocks = blocks;
            v->block = 0; v->read = 0; v->pcm_samples = 0; v->pcm_taken = 0;
            if (v->format.type == WAVEFORM_ATRAC9 && data && blocks && count) {
                if (!v->decoder) v->decoder = Atrac9GetHandle();
                unsigned char cfg[ATRAC9_CONFIG_DATA_SIZE] = {
                    (unsigned char)(v->format.config >> 24), (unsigned char)(v->format.config >> 16),
                    (unsigned char)(v->format.config >> 8), (unsigned char)v->format.config,
                };
                Atrac9CodecInfo info;
                if (v->decoder && !Atrac9InitDecoder(v->decoder, cfg) &&
                    !Atrac9GetCodecInfo(v->decoder, &info)) {
                    v->decode_channels = (uint32_t)info.channels;
                    v->state |= VOICE_STATE_PLAYING;
                    TRACE("    decoder ready: %d ch, %d Hz, superframe %d bytes, %d samples\n",
                          info.channels, info.samplingRate, info.superframeSize,
                          info.frameSamples * info.framesInSuperframe);
                } else {
                    TRACE("    decoder refused this voice's configuration 0x%08x\n", v->format.config);
                }
            }
            TRACE("  waveform voice=%p data=%p flags=0x%x blocks=%u list=%p\n",
                  handle, data, flags, count, (const void *)blocks);
            if (tracing() && data && blocks && count && count < 64) {
                const unsigned char *d = data;
                TRACE("    block0 offset=%u size=%u samples=%u repeats=%u; first bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
                      blocks[0].data_offset, blocks[0].data_size, blocks[0].samples, blocks[0].repeats,
                      d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
            }
            break;
        }
        default: break;
        }
        /* Until every parameter's shape is known, the raw bytes of the ones that matter are
         * the only reliable source: RESOGUN_NGS2_TRACE=2 dumps them. */
        if (tracing() > 1) {
            unsigned shown = h->size > 192 ? 192 : h->size;
            fprintf(stderr,"NGS2:   raw id=0x%08x size=%u next=%d:", h->id, h->size, h->next);
            for (unsigned b = 0; b < shown; ++b) fprintf(stderr," %02x", at[b]);
            if (shown < h->size) fprintf(stderr," ... (%u more)", h->size - shown);
            fprintf(stderr,"\n");
        }
        if (!h->next) break;
        at += h->next;
    }
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
/* The ATRAC9 geometry of a configuration word, from the decoder itself rather than guessed:
 * one superframe is `superframeSize` bytes and holds `framesInSuperframe` frames of
 * `frameSamples` samples. Returns 0 on success. */
static int atrac9_geometry(uint32_t config, Atrac9CodecInfo *out) {
    void *handle = Atrac9GetHandle();
    if (!handle) return -1;
    /* The configuration word is held as a 32-bit value whose top byte is ATRAC9's 0xfe marker,
     * so the decoder's byte array is that value most significant byte first. */
    unsigned char bytes[ATRAC9_CONFIG_DATA_SIZE] = {
        (unsigned char)(config >> 24), (unsigned char)(config >> 16),
        (unsigned char)(config >> 8), (unsigned char)config,
    };
    int failed = Atrac9InitDecoder(handle, bytes) || Atrac9GetCodecInfo(handle, out);
    Atrac9ReleaseHandle(handle);
    return failed ? -1 : 0;
}

/* sceNgs2GetWaveformFrameInfo(format, outFrameSize, outFrameSamples, outUnitsPerFrame,
 * outDelaySamples): how one frame of a waveform format is laid out. The game sizes its own
 * buffers from this, so a guess here makes it ask Scream for something impossible.
 * ATRAC9 (type 0x40) is answered by the decoder; the PCM types are a sample a frame. */
static ABI int32_t waveform_frame_info(const void *format, uint32_t *frame_size,
                                       uint32_t *frame_samples, uint32_t *units_per_frame,
                                       uint32_t *delay_samples) {
    if (!format) return ERR_INVALID_BUFFER_ADDRESS;
    const WaveformFormat *f = format;
    uint32_t channels = f->channels ? f->channels : 1;
    uint32_t size, samples, units, delay = 0;
    if (f->type == WAVEFORM_ATRAC9) {
        Atrac9CodecInfo info;
        if (atrac9_geometry(f->config, &info)) {
            /* Refusing here would leave the caller with no frame size at all, and the engine
             * waits on one; the ATRAC9 default of a 256-sample frame keeps it moving. */
            TRACE("GetWaveformFrameInfo: ATRAC9 config 0x%08x refused by the decoder; answering the default\n",
                  f->config);
            size = 0; samples = 256; units = 1; delay = 0;
        } else {
            size = (uint32_t)info.superframeSize;
            samples = (uint32_t)(info.frameSamples * info.framesInSuperframe);
            units = (uint32_t)info.framesInSuperframe;
            delay = (uint32_t)info.frameSamples;   /* the decoder's own priming frame */
        }
    } else {
        uint32_t bytes = (f->type == WAVEFORM_PCM_I16_LE || f->type == WAVEFORM_PCM_I16_BE) ? 2 : 4;
        size = bytes * channels; samples = 1; units = 1;
    }
    if (frame_size) *frame_size = size;
    if (frame_samples) *frame_samples = samples;
    if (units_per_frame) *units_per_frame = units;
    if (delay_samples) *delay_samples = delay;
    TRACE("GetWaveformFrameInfo(type=0x%x, channels=%u, rate=%u, config=0x%08x) -> size=%u samples=%u units=%u delay=%u\n",
          f->type, channels, f->sample_rate, f->config, size, samples, units, delay);
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
    printf("Runtime: NGS2 systems=%zu, racks=%zu, voice handles=%zu, renders=%zu, "
           "%zu bytes rendered, ATRAC9 superframes decoded=%zu, voice mixes=%zu\n",
           systems, racks, voices_handed, renders, rendered_bytes, decoded_frames, mixed_voices);
}
