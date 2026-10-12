/* sound.c — `armrecomp at9`: an ATRAC9 file decoded.
 *
 * A title's music and speech are .at9 files, and the runtime decodes them
 * as the title plays them (src/atrac9.c, by way of src/hle_ngs.c). This
 * decodes one whole, with the same decoder, to a .wav another program can
 * play or compare: the sound as the file says it is, which is what was
 * decoded less the decoder's delay at the front and the last superframe's
 * padding at the back. With --all nothing is cut, and the output is every
 * superframe's samples, which is the form to compare decoders in.
 */

#include "atrac9.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint8_t *whole_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    long n = 0;
    if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET)) {
        data = (uint8_t *)malloc((size_t)n + 1);
        if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    }
    if (f) fclose(f);
    *size = (size_t)n;
    return data;
}

int cmd_at9(const char *in_path, const char *out_path, int all) {
    size_t size = 0;
    uint8_t *file = whole_file(in_path, &size);
    atrac9_file at;
    static atrac9_decoder dec;
    if (!file) { fprintf(stderr, "armrecomp: cannot read %s\n", in_path); return 1; }
    if (!atrac9_file_open(file, size, &at) || !atrac9_open(&dec, at.config)) {
        fprintf(stderr, "armrecomp: %s is not an ATRAC9 file\n", in_path);
        free(file);
        return 1;
    }

    const atrac9_info *info = &dec.info;
    const size_t superframes = at.data_bytes / info->superframe_bytes;
    const size_t decoded = superframes * info->superframe_samples;
    size_t skip = 0, keep = decoded;
    if (!all) {
        skip = at.delay < decoded ? at.delay : decoded;
        keep = decoded - skip;
        if (at.samples && at.samples < keep) keep = at.samples;
    }

    printf("%s\n", in_path);
    printf("  configuration   %02X %02X %02X %02X\n", at.config[0], at.config[1], at.config[2], at.config[3]);
    printf("  sound           %u Hz, %u channel%s, %u kbit/s\n", info->rate, info->channels, info->channels == 1 ? "" : "s",
           (unsigned)((uint64_t)info->superframe_bytes * 8u * info->rate / info->superframe_samples / 1000u));
    printf("  superframes     %zu of %u bytes: %u frame%s of %u samples\n", superframes, info->superframe_bytes,
           info->frames, info->frames == 1 ? "" : "s", info->frame_samples);
    printf("  length          %u samples after a delay of %u\n", at.samples, at.delay);
    if (at.loops) printf("  loop            %u to %u\n", at.loop_start, at.loop_end);

    float *block = (float *)malloc(sizeof(float) * info->superframe_samples * info->channels);
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * (decoded ? decoded : 1) * info->channels);
    if (!block || !pcm) { fprintf(stderr, "armrecomp: out of memory\n"); return 1; }

    size_t refused = 0;
    const clock_t before = clock();
    for (size_t i = 0; i < superframes; i++) {
        if (!atrac9_decode(&dec, file + at.data + i * info->superframe_bytes, block)) {
            if (refused++ < 8) printf("  superframe %zu is not a valid one: silence\n", i);
        }
        atrac9_pcm16(block, pcm + i * info->superframe_samples * info->channels, (size_t)info->superframe_samples * info->channels);
    }
    const double took = (double)(clock() - before) / CLOCKS_PER_SEC;
    const double seconds = (double)decoded / info->rate;
    printf("  decoded         %zu samples (%.2f s) in %.3f s", decoded, seconds, took);
    if (took > 0) printf(": %.0f times as fast as it plays", seconds / took);
    printf("\n");
    if (refused) printf("  refused         %zu superframe%s\n", refused, refused == 1 ? "" : "s");

    int failed = 0;
    if (out_path) {
        FILE *f = fopen(out_path, "wb");
        uint8_t head[44];
        const uint32_t bytes = (uint32_t)(keep * info->channels * 2u);
        memcpy(head, "RIFF", 4); put32(head + 4, 36 + bytes); memcpy(head + 8, "WAVEfmt ", 8);
        put32(head + 16, 16); put16(head + 20, 1); put16(head + 22, info->channels);
        put32(head + 24, info->rate); put32(head + 28, info->rate * info->channels * 2u);
        put16(head + 32, info->channels * 2u); put16(head + 34, 16);
        memcpy(head + 36, "data", 4); put32(head + 40, bytes);
        if (!f || fwrite(head, 1, sizeof head, f) != sizeof head ||
            fwrite(pcm + skip * info->channels, 2, keep * info->channels, f) != keep * info->channels) {
            fprintf(stderr, "armrecomp: cannot write %s\n", out_path);
            failed = 1;
        } else {
            printf("  written         %zu samples to %s\n", keep, out_path);
        }
        if (f) fclose(f);
    }
    free(block);
    free(pcm);
    free(file);
    return failed || refused ? 1 : 0;
}
