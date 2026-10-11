/* gpu_none.c — gpu.h where there is no implementation of it yet: there is
 * no card, gpu_open says so, and every list is drawn on the processor. */

#include "gpu.h"

int gpu_open(void) { return -1; }
const char *gpu_name(void) { return "nothing"; }

gpu_shader *gpu_shader_make(int fragment, const char *hlsl, unsigned pa_words, char *log, size_t log_size) {
    (void)fragment; (void)hlsl; (void)pa_words;
    if (log && log_size) log[0] = '\0';
    return NULL;
}
void gpu_shader_free(gpu_shader *s) { (void)s; }

gpu_texture *gpu_texture_make(unsigned format, unsigned width, unsigned height, unsigned levels, int cube,
                              const void *const *images) {
    (void)format; (void)width; (void)height; (void)levels; (void)cube; (void)images;
    return NULL;
}
void gpu_texture_free(gpu_texture *t) { (void)t; }

int gpu_ordered(unsigned format) { (void)format; return 0; }
int gpu_begin(const gpu_target *t) { (void)t; return -1; }
int gpu_draw(const gpu_draw_call *d) { (void)d; return -1; }
int gpu_end(void *colour, uint32_t colour_pitch, void *depth, uint32_t depth_pitch, uint64_t *counts, unsigned max_counts) {
    (void)colour; (void)colour_pitch; (void)depth; (void)depth_pitch; (void)counts; (void)max_counts;
    return -1;
}
