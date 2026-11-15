/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NGS2, the PS4's voice mixer (libSceNgs2). Tempest 4000 builds a system, one rack of voices in
 * it, and renders that system into its own buffers, which it then writes to libSceAudioOut.
 *
 * Samplers decode once per render grain, then their port patches route that same grain through
 * submixers to each mastering output. Render buffers are simultaneous outputs, not successive
 * pieces of the waveform: decoding again for each buffer splits a tune into alternate chunks.
 *
 * The structures follow shadPS4's core/libraries/ngs2/ngs2.h (GPL-2.0-or-later), as do the error
 * codes; the game's own allocator is not called, because its handlers are guest code and nothing
 * here needs the memory to come from the guest's pool. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
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
#define MAX_PORTS 16
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
/* A voice parameter's id is (rack id << 16) | index, so the same parameter has a different id
 * on each kind of rack. The constants below follow AnyPS5's libSceNgs2.native (GPL-2.0),
 * whose NGS2 is a working native implementation.
 * This game builds a custom sampler rack (0x4001), a custom submixer (0x4002), a submixer
 * (0x2000) and a mastering rack (0x3000). */
#define RACK_SAMPLER 0x1000u
#define RACK_CUSTOM_SAMPLER 0x4001u
#define RACK_MASTERING 0x3000u
#define PARAM_INDEX_SETUP 0x0000u
#define PARAM_INDEX_ADD_WAVEFORM_BLOCKS 0x0001u
#define PARAM_INDEX_PITCH 0x0005u
#define PARAM_VOICE_EVENT 0x0006u
/* PS4 events are ordinals, unlike the PS5 bit masks. The game's Scream wrappers at eboot
 * 0x14a775, 0x14a86d, 0x14a82e, 0x14a980 and 0x14aa10 send 0, 1, 3, 4 and 5. */
#define EVENT_PLAY 0u
#define EVENT_STOP 1u
#define EVENT_STOP_IMMEDIATE 2u
#define EVENT_KILL 3u
#define EVENT_PAUSE 4u
#define EVENT_RESUME 5u
#define PARAM_PORT_VOLUME 2u
#define PARAM_PATCH 5u
#define PARAM_MASTERING_OUTPUT 0x30000005u
static int is_sampler_param(uint32_t id, uint32_t index) {
    uint32_t rack = id >> 16;
    return (id & 0xffffu) == index && (rack == RACK_SAMPLER || rack == RACK_CUSTOM_SAMPLER);
}
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
#define VOICE_STATE_PAUSED 4u
#define VOICE_STATE_STOPPED 8u
#define VOICE_STATE_EMPTY 0x20u
/* Internal selectors for the two sampler parameters, which have no single id of their own. */
#define PARAM_SAMPLER_SETUP 0xf0000000u
#define PARAM_SAMPLER_WAVEFORM_BLOCKS 0xf0000001u
#define PARAM_SAMPLER_PITCH 0xf0000005u
#define ERR_INVALID_WAVEFORM_TYPE ((int32_t)0x804A0402)

enum { SYSTEM_MAGIC=0x4e475332, RACK_MAGIC=0x4e475233, VOICE_MAGIC=0x4e475234 };

typedef struct Rack Rack;
typedef struct Voice Voice;
struct Voice {
    uint32_t magic, state;
    Rack *rack;
    uint32_t index;
    WaveformFormat format;     /* from PARAM_SAMPLER_SETUP */
    const unsigned char *data; /* from PARAM_SAMPLER_WAVEFORM_BLOCKS */
    uint32_t block_count;
    WaveformBlock *blocks;     /* descriptors are caller scratch, so copy them at submission */
    struct { Voice *dest; float volume; } ports[MAX_PORTS];
    uint32_t output_id;
    /* Decoding state: one ATRAC9 superframe is decoded at a time and handed out a grain at a
     * time, because a grain is shorter than a superframe. */
    void *decoder;
    uint32_t block;            /* which block is playing */
    uint32_t read;             /* bytes consumed from it */
    float pcm[ATRAC9_MAX_SUPERFRAME * 2];
    uint32_t pcm_samples, pcm_taken;
    uint32_t decode_channels;
    int source_ready;
    float pitch;
    double resample_phase;
    float resample_current[2], resample_next[2];
    int resample_started, resample_current_valid, resample_next_valid;
    /* ATRAC9 geometry: a decode call yields one frame, and a superframe holds several of
     * them followed by padding that has to be stepped over. */
    uint32_t frame_samples, frames_in_superframe, superframe_bytes;
    uint32_t frame_index, input_cursor;
    /* A block does not start at its first decoded sample and does not run to the end of what
     * it decodes: numSkipSamples are dropped and only numSamples are played. */
    uint32_t skip_samples, remaining_samples;
    uint32_t repeated;
    int block_started;
};
struct Rack { uint32_t magic; void *system; struct Rack *next; uint32_t voice_count, rack_id; Voice voices[]; };
/* A system keeps its racks so rendering can walk every voice it owns. */
typedef struct { uint32_t magic, sample_rate; Rack *racks; } System;

static size_t systems, racks, voices_handed, renders, rendered_bytes, decoded_frames, mixed_voices;
static size_t pcm_plays, pcm_stops, pcm_ends, pcm_mixes;
/* TEMPEST_NGS2_TRACE=1 logs every call and what it answered: Scream's own error messages name
 * only its source line, so the call that upset it has to be found from this side. */
static int tracing(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("TEMPEST_NGS2_TRACE"); on = v ? atoi(v) : 0; }
    return on;
}
#define TRACE(...) do { if (tracing()) fprintf(stderr,"NGS2: " __VA_ARGS__); } while (0)

/* sceNgs2SystemCreateWithAllocator(option, allocator, outHandle): allocates the system handle. */
static ABI int32_t system_create(const void *option, const void *allocator, void **out) {
    (void)allocator;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    System *s = runtime_low_alloc(sizeof(*s));
    if (!s) return ERR_OUT_OF_MEMORY;
    s->magic = SYSTEM_MAGIC; s->racks = NULL;
    s->sample_rate = 48000;
    if (option) {
        /* OrbisNgs2SystemOption: size, name[16], flags, maxGrain, numGrain, sampleRate. */
        size_t size;
        memcpy(&size, option, sizeof(size));
        if (size >= 40) {
            uint32_t rate;
            memcpy(&rate, (const unsigned char *)option + 36, sizeof(rate));
            if (rate) s->sample_rate = rate;
        }
    }
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
/* Restart the codec at a waveform boundary, including the priming samples of a repeated block. */
static int start_decoder(Voice *v) {
    v->source_ready = 0;
    if (v->format.type == WAVEFORM_PCM_I16_LE || v->format.type == WAVEFORM_PCM_I16_BE ||
        v->format.type == WAVEFORM_PCM_F32_LE || v->format.type == WAVEFORM_PCM_F32_BE) {
        v->decode_channels = v->format.channels ? v->format.channels : 1;
        return v->source_ready = v->decode_channels <= 2;
    }
    if (v->format.type != WAVEFORM_ATRAC9) return 0;
    /* InitDecoder does not clear LibAtrac9's superframe cursor or overlap history.
     * A loop can end partway through a superframe: reusing that state rejects the
     * first frame on the next pass, silencing the title music after one loop. */
    if (v->decoder) Atrac9ReleaseHandle(v->decoder);
    v->decoder = Atrac9GetHandle();
    unsigned char cfg[ATRAC9_CONFIG_DATA_SIZE] = {
        (unsigned char)(v->format.config >> 24), (unsigned char)(v->format.config >> 16),
        (unsigned char)(v->format.config >> 8), (unsigned char)v->format.config,
    };
    Atrac9CodecInfo info;
    if (!v->decoder || Atrac9InitDecoder(v->decoder, cfg) ||
        Atrac9GetCodecInfo(v->decoder, &info) || info.channels < 1 || info.channels > 2)
        return 0;
    v->decode_channels = (uint32_t)info.channels;
    v->frame_samples = (uint32_t)info.frameSamples;
    v->frames_in_superframe = (uint32_t)info.framesInSuperframe;
    v->superframe_bytes = (uint32_t)info.superframeSize;
    v->frame_index = v->input_cursor = 0;
    return v->source_ready = 1;
}

static void rewind_waveform(Voice *v) {
    v->block = v->read = v->repeated = 0;
    v->pcm_samples = v->pcm_taken = 0;
    v->skip_samples = v->remaining_samples = 0;
    v->block_started = 0;
    v->resample_phase = 0;
    v->resample_started = v->resample_current_valid = v->resample_next_valid = 0;
    start_decoder(v);
}

/* PCM is stored in waveform blocks without a codec handle. Read whole interleaved frames;
 * an incomplete final sample never borrows bytes from the following allocation. */
static uint32_t pcm_decode(Voice *v, const unsigned char *at, uint32_t bytes) {
    uint32_t sample_bytes = (v->format.type == WAVEFORM_PCM_I16_LE ||
                             v->format.type == WAVEFORM_PCM_I16_BE) ? 2 : 4;
    uint32_t frames = bytes / (sample_bytes * v->decode_channels);
    if (frames > ATRAC9_MAX_SUPERFRAME) frames = ATRAC9_MAX_SUPERFRAME;
    int big_endian = v->format.type == WAVEFORM_PCM_I16_BE || v->format.type == WAVEFORM_PCM_F32_BE;
    for (uint32_t i = 0; i < frames * v->decode_channels; ++i) {
        const unsigned char *p = at + (size_t)i * sample_bytes;
        if (sample_bytes == 2) {
            uint16_t bits = big_endian ? (uint16_t)((p[0] << 8) | p[1]) :
                                         (uint16_t)(p[0] | (p[1] << 8));
            v->pcm[i] = (float)(int16_t)bits / 32768.0f;
        } else {
            uint32_t bits = big_endian ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                           ((uint32_t)p[2] << 8) | p[3] :
                                         p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                                           ((uint32_t)p[3] << 24);
            memcpy(&v->pcm[i], &bits, sizeof(bits));
        }
    }
    return frames;
}

/* The next `want` samples a channel of a voice, decoded as needed, into `out` interleaved by
 * the voice's own channel count. Returns how many samples a channel were produced. */
static uint32_t voice_samples(Voice *v, float *out, uint32_t want) {
    if (!v->source_ready || !v->data || !v->blocks || v->block >= v->block_count) return 0;
    uint32_t done = 0;
    while (done < want) {
        if (v->block >= v->block_count) break;
        const WaveformBlock *b = &v->blocks[v->block];
        if (!v->block_started) {
            v->skip_samples = b->skip_samples;
            v->remaining_samples = b->samples ? b->samples : UINT32_MAX;
            v->block_started = 1;
        }
        /* A sample window can finish inside the last decoded frame. Discard that tail before
         * deciding whether to repeat or advance; otherwise the voice gets stuck on take == 0. */
        if (!v->remaining_samples || (v->read >= b->data_size && v->pcm_taken >= v->pcm_samples)) {
            uint32_t frame_bytes = v->decode_channels *
                ((v->format.type == WAVEFORM_PCM_I16_LE || v->format.type == WAVEFORM_PCM_I16_BE) ? 2 : 4);
            int can_repeat = b->data_size && (v->format.type == WAVEFORM_ATRAC9 ||
                                               b->data_size / frame_bytes > b->skip_samples);
            if (can_repeat && (b->repeats == UINT32_MAX || v->repeated < b->repeats)) {
                if (b->repeats != UINT32_MAX) ++v->repeated;
                if (!start_decoder(v)) break;
            } else {
                ++v->block; v->repeated = 0;
            }
            v->read = 0; v->block_started = 0;
            v->pcm_samples = v->pcm_taken = 0;
            v->frame_index = v->input_cursor = 0;
            continue;
        }
        if (v->pcm_taken < v->pcm_samples) {
            /* Drop the block's lead-in before anything is handed out. */
            if (v->skip_samples) {
                uint32_t drop = v->pcm_samples - v->pcm_taken;
                if (drop > v->skip_samples) drop = v->skip_samples;
                v->pcm_taken += drop; v->skip_samples -= drop;
                continue;
            }
            uint32_t take = v->pcm_samples - v->pcm_taken;
            if (take > want - done) take = want - done;
            if (v->remaining_samples != UINT32_MAX && take > v->remaining_samples)
                take = v->remaining_samples;
            if (!take) break;
            memcpy(out + (size_t)done * v->decode_channels,
                   v->pcm + (size_t)v->pcm_taken * v->decode_channels,
                   (size_t)take * v->decode_channels * sizeof(float));
            v->pcm_taken += take; done += take;
            if (v->remaining_samples != UINT32_MAX) v->remaining_samples -= take;
            continue;
        }
        /* Refill from the block that is playing; move on when it runs out. */
        const unsigned char *at = v->data + b->data_offset + v->read;
        if (v->format.type != WAVEFORM_ATRAC9) {
            v->pcm_samples = pcm_decode(v, at, b->data_size - v->read);
            v->pcm_taken = 0;
            if (!v->pcm_samples) { v->read = b->data_size; continue; }
            uint32_t sample_bytes = (v->format.type == WAVEFORM_PCM_I16_LE ||
                                     v->format.type == WAVEFORM_PCM_I16_BE) ? 2 : 4;
            v->read += v->pcm_samples * v->decode_channels * sample_bytes;
            continue;
        }
        int used = 0;
        if (Atrac9DecodeF32(v->decoder, at, (int)(b->data_size - v->read), v->pcm, &used, 0) || used <= 0)
            break;
        /* One call decodes one frame, not a whole superframe. */
        v->pcm_samples = v->frame_samples;
        v->pcm_taken = 0;
        v->read += (uint32_t)used;
        v->input_cursor += (uint32_t)used;
        /* A superframe ends with padding the decoder does not consume; step over it so the
         * next frame starts where the encoder put it. */
        if (v->frames_in_superframe && ++v->frame_index >= v->frames_in_superframe) {
            if (v->superframe_bytes > v->input_cursor)
                v->read += v->superframe_bytes - v->input_cursor;
            v->frame_index = 0; v->input_cursor = 0;
        }
        decoded_frames++;
    }
    return done;
}

/* Sampler pitch is relative to format.sampleRate. Keep the lookahead and fractional position
 * between renders so 44.1 kHz effects do not restart their resampling phase every grain. */
static uint32_t render_voice_samples(Voice *v, float *out, uint32_t want, uint32_t output_rate) {
    const double rate = v->format.sample_rate ? v->format.sample_rate : output_rate;
    const double step = rate * v->pitch / output_rate;
    if (!v->resample_started && step == 1.0) return voice_samples(v, out, want);
    if (!v->resample_started) {
        v->resample_current_valid = voice_samples(v, v->resample_current, 1) != 0;
        v->resample_next_valid = voice_samples(v, v->resample_next, 1) != 0;
        v->resample_started = 1;
    }
    uint32_t done = 0;
    while (done < want && v->resample_current_valid) {
        for (uint32_t c = 0; c < v->decode_channels; ++c) {
            float a = v->resample_current[c];
            float b = v->resample_next_valid ? v->resample_next[c] : a;
            out[(size_t)done * v->decode_channels + c] = a + (b - a) * (float)v->resample_phase;
        }
        ++done;
        v->resample_phase += step;
        while (v->resample_phase >= 1.0 && v->resample_current_valid) {
            v->resample_phase -= 1.0;
            memcpy(v->resample_current, v->resample_next, sizeof(v->resample_current));
            v->resample_current_valid = v->resample_next_valid;
            v->resample_next_valid = voice_samples(v, v->resample_next, 1) != 0;
        }
    }
    return done;
}

/* Adds a voice's samples into an interleaved output buffer. Channels beyond the voice's own are
 * left alone, so a stereo voice lands on the first two of a surround buffer. */
static void mix_into(const float *source, uint32_t source_channels, uint32_t samples,
                     void *buffer, uint32_t type, uint32_t channels, float gain) {
    for (uint32_t i = 0; i < samples; ++i)
        for (uint32_t c = 0; c < channels && c < (source_channels == 1 ? 2 : source_channels); ++c) {
            /* The decoder's full scale is 32768; the output's is 32767. */
            float value = source[(size_t)i * source_channels + (source_channels == 1 ? 0 : c)] *
                          gain * (32767.0f / 32768.0f);
            if (value > 1.0f) value = 1.0f; else if (value < -1.0f) value = -1.0f;
            size_t at = (size_t)i * channels + c;
            if (type == WAVEFORM_PCM_F32_LE || type == WAVEFORM_PCM_F32_BE) {
                ((float *)buffer)[at] += value;
            } else {
                int32_t sum = ((int16_t *)buffer)[at] + (int32_t)(value * 32767.0f);
                ((int16_t *)buffer)[at] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
            }
        }
}

/* Follow the port patches to a mastering output. A bounded walk also cuts feedback paths; this
 * game's racks use a sampler -> submixer -> mastering graph and zero-volume effect sends. */
static float output_gain(const Voice *v, uint32_t output, unsigned depth) {
    if (!v || v->magic != VOICE_MAGIC || depth >= MAX_PORTS ||
        !(v->state & VOICE_STATE_PLAYING) || (v->state & VOICE_STATE_PAUSED)) return 0.0f;
    if (v->rack->rack_id == RACK_MASTERING) return v->output_id == output ? 1.0f : 0.0f;
    float gain = 0.0f;
    for (unsigned p = 0; p < MAX_PORTS; ++p)
        if (v->ports[p].dest && v->ports[p].volume != 0.0f)
            gain += v->ports[p].volume * output_gain(v->ports[p].dest, output, depth + 1);
    return gain;
}

/* Bounded opt-in diagnostics for effects that stop after extended play. Show whether the
 * engine still starts PCM voices, whether they finish, and whether a playing voice is routed. */
static void diagnose_voices(const System *s, uint32_t outputs) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("TEMPEST_NGS2_DIAG");
        enabled = env && env[0] == '1';
    }
    if (!enabled || (renders > 3 && renders % 500)) return;
    unsigned playing = 0, paused = 0, stopped = 0, empty = 0, routed = 0, music = 0;
    for (const Rack *r = s->racks; r; r = r->next)
        for (uint32_t n = 0; n < r->voice_count; ++n) {
            const Voice *v = &r->voices[n];
            if (v->format.type == WAVEFORM_ATRAC9) {
                music += !!(v->state & VOICE_STATE_PLAYING);
                continue;
            }
            if (!v->source_ready) continue;
            playing += !!(v->state & VOICE_STATE_PLAYING);
            paused += !!(v->state & VOICE_STATE_PAUSED);
            stopped += !!(v->state & VOICE_STATE_STOPPED);
            empty += !!(v->state & VOICE_STATE_EMPTY);
            for (uint32_t i = 0; i < outputs; ++i)
                if (output_gain(v, i, 0) != 0.0f) { ++routed; break; }
        }
    fprintf(stderr, "NGS2 diagnostic: render=%zu music=%u PCM playing=%u paused=%u stopped=%u "
            "empty=%u routed=%u; starts=%zu stops=%zu ends=%zu mixes=%zu\n",
            renders, music, playing, paused, stopped, empty, routed,
            pcm_plays, pcm_stops, pcm_ends, pcm_mixes);
}

/* sceNgs2SystemRender(handle, buffers, count): all buffers receive the same point in time. */
static ABI int32_t system_render(void *handle, const RenderBufferInfo *buffers, uint32_t count) {
    System *s = handle;
    if (!s || s->magic != SYSTEM_MAGIC) return ERR_INVALID_SYSTEM_HANDLE;
    if (count && !buffers) return ERR_INVALID_BUFFER_ADDRESS;
    float scratch[ATRAC9_MAX_SUPERFRAME * 2];
    uint32_t grain = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (renders < 3)
            TRACE("  output %u type=0x%x channels=%u bytes=%llu\n", i,
                  buffers[i].waveform_type, buffers[i].channels,
                  (unsigned long long)buffers[i].buffer_size);
        if (!buffers[i].buffer || !buffers[i].buffer_size) continue;
        memset(buffers[i].buffer, 0, (size_t)buffers[i].buffer_size);
        uint32_t channels = buffers[i].channels ? buffers[i].channels : 2;
        uint32_t bytes = (buffers[i].waveform_type == WAVEFORM_PCM_I16_LE ||
                          buffers[i].waveform_type == WAVEFORM_PCM_I16_BE) ? 2 : 4;
        uint64_t frames = buffers[i].buffer_size / (bytes * channels);
        if (frames > ATRAC9_MAX_SUPERFRAME) frames = ATRAC9_MAX_SUPERFRAME;
        if (frames > grain) grain = (uint32_t)frames;
        rendered_bytes += (size_t)buffers[i].buffer_size;
    }
    /* Advance each sampler once, then fan its decoded grain out to the patched destinations. */
    for (Rack *r = s->racks; grain && r; r = r->next)
        for (uint32_t n = 0; n < r->voice_count; ++n) {
            Voice *v = &r->voices[n];
            if (!v->source_ready || !(v->state & VOICE_STATE_PLAYING) ||
                (v->state & VOICE_STATE_PAUSED)) continue;
            uint32_t produced = render_voice_samples(v, scratch, grain, s->sample_rate);
            if (!produced) {
                v->state = (v->state & ~VOICE_STATE_PLAYING) | VOICE_STATE_EMPTY;
                if (v->format.type != WAVEFORM_ATRAC9) ++pcm_ends;
                continue;
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (!buffers[i].buffer || !buffers[i].buffer_size) continue;
                float gain = output_gain(v, i, 0);
                if (gain == 0.0f) continue;
                uint32_t channels = buffers[i].channels ? buffers[i].channels : 2;
                uint32_t bytes = (buffers[i].waveform_type == WAVEFORM_PCM_I16_LE ||
                                  buffers[i].waveform_type == WAVEFORM_PCM_I16_BE) ? 2 : 4;
                uint64_t frames = buffers[i].buffer_size / (bytes * channels);
                uint32_t samples = frames < produced ? (uint32_t)frames : produced;
                mix_into(scratch, v->decode_channels, samples, buffers[i].buffer,
                         buffers[i].waveform_type, channels, gain);
            }
            mixed_voices++;
            if (v->format.type != WAVEFORM_ATRAC9) ++pcm_mixes;
        }
    ++renders;
    diagnose_voices(s, count);
    if (renders <= 3 || !(renders % 500))
        TRACE("SystemRender(system=%p, buffers=%u) #%zu\n", handle, count, renders);
    return 0;
}
/* sceNgs2RackCreateWithAllocator(system, rackId, option, allocator, outHandle): a rack is a bank
 * of voices of one kind. Its option contains the maximum number of voices. */
static ABI int32_t rack_create(void *system, uint32_t rack_id, const void *option,
                               const void *allocator, void **out) {
    (void)allocator;
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
    r->magic = RACK_MAGIC; r->system = s; r->voice_count = count; r->rack_id = rack_id;
    r->next = s->racks; s->racks = r;
    for (uint32_t i = 0; i < count; ++i) {
        r->voices[i].magic = VOICE_MAGIC; r->voices[i].state = 0;
        r->voices[i].rack = r; r->voices[i].index = i;
        r->voices[i].pitch = 1.0f;
        for (unsigned p = 0; p < MAX_PORTS; ++p) r->voices[i].ports[p].volume = 1.0f;
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
        {
            if (r->voices[i].decoder) { Atrac9ReleaseHandle(r->voices[i].decoder); r->voices[i].decoder = NULL; }
            free(r->voices[i].blocks); r->voices[i].blocks = NULL;
            r->voices[i].magic = 0;
        }
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
 * a matrix, play, stop). Event values are PS4 ordinals, not a bit mask. */
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
        if (h->id == PARAM_VOICE_EVENT && h->size >= sizeof(*h) + 4) {
            uint32_t event;
            memcpy(&event, at + sizeof(*h), 4);
            TRACE("  event voice=%p 0x%x%s\n", handle, event,
                  event == EVENT_PLAY ? " (play)" : event == EVENT_STOP ? " (stop)" :
                  event == EVENT_PAUSE ? " (pause)" : event == EVENT_RESUME ? " (resume)" : "");
            switch (event) {
            case EVENT_PLAY:
                if (v->format.type != WAVEFORM_ATRAC9) ++pcm_plays;
                if (!(v->state & VOICE_STATE_PLAYING)) rewind_waveform(v);
                v->state = VOICE_STATE_IN_USE | VOICE_STATE_PLAYING;
                break;
            case EVENT_STOP: case EVENT_STOP_IMMEDIATE: case EVENT_KILL:
                /* Scream releases its pool slot only for zero state or EMPTY (eboot 0x142079).
                 * With no release envelope implemented, stopping completes the voice now. */
                v->state = VOICE_STATE_IN_USE | VOICE_STATE_STOPPED | VOICE_STATE_EMPTY;
                if (v->format.type != WAVEFORM_ATRAC9) ++pcm_stops;
                break;
            case EVENT_PAUSE: v->state |= VOICE_STATE_PAUSED; break;
            case EVENT_RESUME: v->state &= ~VOICE_STATE_PAUSED; break;
            default: break;
            }
        }
        switch (is_sampler_param(h->id, PARAM_INDEX_SETUP) ? PARAM_SAMPLER_SETUP :
                is_sampler_param(h->id, PARAM_INDEX_ADD_WAVEFORM_BLOCKS) ? PARAM_SAMPLER_WAVEFORM_BLOCKS :
                is_sampler_param(h->id, PARAM_INDEX_PITCH) ? PARAM_SAMPLER_PITCH :
                h->id) {
        case PARAM_SAMPLER_PITCH: {
            float ratio;
            if (h->size < sizeof(*h) + sizeof(ratio)) break;
            memcpy(&ratio, at + sizeof(*h), sizeof(ratio));
            if (isfinite(ratio) && ratio > 0.0f) v->pitch = ratio;
            break;
        }
        case PARAM_PORT_VOLUME: {
            if (h->size < sizeof(*h) + 8) break;
            uint32_t port; float volume;
            memcpy(&port, at + sizeof(*h), 4); memcpy(&volume, at + sizeof(*h) + 4, 4);
            if (port < MAX_PORTS) v->ports[port].volume = volume;
            break;
        }
        case PARAM_PATCH: {
            if (h->size < sizeof(*h) + 16) break;
            uint32_t port; Voice *dest;
            memcpy(&port, at + sizeof(*h), 4); memcpy(&dest, at + sizeof(*h) + 8, sizeof(dest));
            if (port < MAX_PORTS) v->ports[port].dest = dest;
            TRACE("  patch voice=%p port=%u dest=%p\n", handle, port, (void *)dest);
            break;
        }
        case PARAM_MASTERING_OUTPUT:
            if (h->size >= sizeof(*h) + 4) memcpy(&v->output_id, at + sizeof(*h), 4);
            break;
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
            WaveformBlock *copy = NULL;
            if (count && blocks) {
                copy = calloc(count, sizeof(*copy));
                if (!copy) return ERR_OUT_OF_MEMORY;
                memcpy(copy, blocks, (size_t)count * sizeof(*copy));
            }
            free(v->blocks);
            v->data = data; v->block_count = copy ? count : 0; v->blocks = copy;
            rewind_waveform(v);
            v->state &= ~VOICE_STATE_EMPTY;
            TRACE("  waveform voice=%p data=%p flags=0x%x blocks=%u list=%p\n",
                  handle, data, flags, count, (const void *)blocks);
            /* The first bytes of the sound identify it: searching the bank for them says which
             * of its tracks the engine actually handed over. */
            if (tracing() && data && blocks && count) {
                const unsigned char *d = (const unsigned char *)data + blocks[0].data_offset;
                fprintf(stderr, "NGS2:     fingerprint:");
                for (int i = 0; i < 24; ++i) fprintf(stderr, " %02x", d[i]);
                fprintf(stderr, "\n");
            }
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
         * the only reliable source: TEMPEST_NGS2_TRACE=2 dumps them. */
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
