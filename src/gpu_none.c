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

gpu_mesh *gpu_mesh_make(const void *vertices, uint32_t vertex_count, uint32_t stride, const uint32_t *indices, uint32_t index_count) {
    (void)vertices; (void)vertex_count; (void)stride; (void)indices; (void)index_count;
    return NULL;
}
void gpu_mesh_free(gpu_mesh *m) { (void)m; }

gpu_surface *gpu_surface_make(unsigned format, uint32_t width, uint32_t height) { (void)format; (void)width; (void)height; return NULL; }
void gpu_surface_free(gpu_surface *s) { (void)s; }
int gpu_surface_put(gpu_surface *s, const void *rows, uint32_t pitch) { (void)s; (void)rows; (void)pitch; return -1; }
int gpu_surface_get(gpu_surface *s, void *rows, uint32_t pitch) { (void)s; (void)rows; (void)pitch; return -1; }
void gpu_surface_clear(gpu_surface *s, float depth, uint8_t stencil) { (void)s; (void)depth; (void)stencil; }
int gpu_surface_copy(gpu_surface *into, gpu_surface *from) { (void)into; (void)from; return -1; }
int gpu_surface_copy_corner(gpu_surface *into, gpu_surface *from, uint32_t width, uint32_t height) {
    (void)into; (void)from; (void)width; (void)height;
    return -1;
}
int gpu_surface_depth_corner(gpu_surface *into, gpu_surface *from, uint8_t stencil) { (void)into; (void)from; (void)stencil; return -1; }

gpu_texture *gpu_texture_blank(unsigned width, unsigned height) { (void)width; (void)height; return NULL; }
int gpu_convert(gpu_texture *into, gpu_surface *from, gpu_shader *program, const void *constants, uint32_t constants_bytes) {
    (void)into; (void)from; (void)program; (void)constants; (void)constants_bytes;
    return -1;
}

int gpu_ordered(unsigned format) { (void)format; return 0; }
int gpu_begin(gpu_surface *colour, gpu_surface *depth) { (void)colour; (void)depth; return -1; }
int gpu_draw(const gpu_draw_call *d) { (void)d; return -1; }
int gpu_end(uint64_t *counts, unsigned max_counts) { (void)counts; (void)max_counts; return -1; }
