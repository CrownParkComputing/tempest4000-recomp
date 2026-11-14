/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Decodes ATRAC9 out of the game's Scream bank into a WAV, so a tune can be listened to and
 * identified outside the game.
 *
 *     extract_at9 <bank> <offset> <bytes> [config-hex] [out.wav]
 *
 * The configuration word defaults to this game's (0xfe740bf0, 48 kHz stereo), which is what its
 * sampler voices are set up with. Decoding stops at the first frame the decoder refuses, which is
 * how the end of a tune shows itself when the bank's own index is not understood. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libatrac9.h"

static void put32(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <bank> <offset> <bytes> [config-hex] [out.wav]\n", argv[0]); return 2; }
    const char *bank = argv[1];
    long long offset = strtoll(argv[2], NULL, 0);
    long long bytes = strtoll(argv[3], NULL, 0);
    unsigned config = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 16) : 0xfe740bf0u;
    const char *out_path = argc > 5 ? argv[5] : "tune.wav";

    FILE *in = fopen(bank, "rb");
    if (!in) { perror(bank); return 1; }
    unsigned char *data = malloc((size_t)bytes);
    if (!data || fseek(in, offset, SEEK_SET) || fread(data, 1, (size_t)bytes, in) != (size_t)bytes) {
        fprintf(stderr, "cannot read %lld bytes at %lld\n", bytes, offset); return 1;
    }
    fclose(in);

    void *decoder = Atrac9GetHandle();
    unsigned char cfg[ATRAC9_CONFIG_DATA_SIZE] = {
        (unsigned char)(config >> 24), (unsigned char)(config >> 16),
        (unsigned char)(config >> 8), (unsigned char)config };
    Atrac9CodecInfo info;
    if (!decoder || Atrac9InitDecoder(decoder, cfg) || Atrac9GetCodecInfo(decoder, &info)) {
        fprintf(stderr, "the decoder refuses configuration 0x%08x\n", config); return 1;
    }
    printf("%d ch, %d Hz, superframe %d bytes of %d frames, %d samples a frame\n",
           info.channels, info.samplingRate, info.superframeSize, info.framesInSuperframe, info.frameSamples);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }
    for (int i = 0; i < 44; ++i) fputc(0, out);          /* the header is written at the end */

    short *pcm = malloc((size_t)info.frameSamples * info.channels * sizeof(short));
    long long at = 0, frames = 0, samples = 0;
    int frame_in_superframe = 0, cursor = 0;
    while (at < bytes) {
        int used = 0;
        if (Atrac9Decode(decoder, data + at, (int)(bytes - at), pcm, &used, 0) || used <= 0) break;
        fwrite(pcm, sizeof(short), (size_t)info.frameSamples * info.channels, out);
        at += used; cursor += used; samples += info.frameSamples; ++frames;
        if (++frame_in_superframe >= info.framesInSuperframe) {   /* step over the superframe padding */
            if (info.superframeSize > cursor) at += info.superframeSize - cursor;
            frame_in_superframe = 0; cursor = 0;
        }
    }
    long long pcm_bytes = samples * info.channels * 2;
    fseek(out, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, out); put32(out, (unsigned)(36 + pcm_bytes)); fwrite("WAVEfmt ", 1, 8, out);
    put32(out, 16); put16(out, 1); put16(out, (unsigned)info.channels);
    put32(out, (unsigned)info.samplingRate);
    put32(out, (unsigned)(info.samplingRate * info.channels * 2));
    put16(out, (unsigned)(info.channels * 2)); put16(out, 16);
    fwrite("data", 1, 4, out); put32(out, (unsigned)pcm_bytes);
    fclose(out);
    printf("%lld frames, %lld samples (%.1f s), %lld of %lld bytes consumed -> %s\n",
           frames, samples, (double)samples / info.samplingRate, at, bytes, out_path);
    Atrac9ReleaseHandle(decoder);
    return 0;
}
