/* hle_gxm.c — SceGxm, the graphics library: the state a title builds up,
 * and the scenes it draws.
 *
 * The library is mostly bookkeeping. A title describes textures and
 * surfaces in small structures of its own, registers compiled shader
 * programs and asks for vertex and fragment programs made from them, sets
 * a few dozen pieces of raster state, binds buffers and textures, and then
 * draws; nearly every call only records something. This file is that
 * record, kept the way the title can see it:
 *
 *   - textures are the hardware's four control words, because titles copy
 *     them about and read them back field by field;
 *   - colour and depth surfaces are structures in the title's memory, of
 *     the sizes the SDK gives them, holding everything a scene needs,
 *     because a title copies those too;
 *   - precomputed draws and states live in the memory the title supplies
 *     for them, for the same reason;
 *   - programs are read in place from the title's own GXP data.
 *
 * What is drawn is drawn at once, by gxm_draw.c, into the colour surface's
 * own memory: a scene is finished when sceGxmEndScene returns, so every
 * notification is already true and nothing waits. Depth, stencil and the
 * mask bit are kept beside the scene and only touch the title's depth
 * surface when it asks for them to be loaded or stored.
 *
 * The layouts of the SDK's opaque structures are this runtime's own where
 * the hardware's are unknown; their sizes, and the fields a title can
 * reach, are the SDK's. Calling convention: see hle.h. Arguments past r3
 * are on the stack and floats arrive in the VFP registers.
 */

#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"
#include "gxm_draw.h"
#include "gxm_tex.h"
#include "hle_sysmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define E_OUT_OF_MEMORY    0x805B0002u
#define E_INVALID_VALUE    0x805B0003u
#define E_INVALID_POINTER  0x805B0004u
#define E_INVALID_ALIGN    0x805B0005u
#define E_NOT_WITHIN_SCENE 0x805B0006u
#define E_WITHIN_SCENE     0x805B0007u
#define E_NULL_PROGRAM     0x805B0008u
#define E_UNSUPPORTED      0x805B0009u

static uint32_t arg(int n) { return vita_read32(sp + 4u * (uint32_t)(n - 4)); }   /* the nth argument, n >= 4 */

/* A small object in guest memory that the title treats as opaque. */
static uint32_t object(uint32_t size) {
    static uint32_t pool, pool_left;
    size = (size + 15) & ~15u;
    if (pool_left < size) {
        pool = vita_sysmem_reserve(0x10000);
        pool_left = pool ? 0x10000 : 0;
    }
    if (!pool) return 0;
    uint32_t at = pool;
    pool += size; pool_left -= size;
    return at;
}

static uint32_t rd(uint32_t at) { return vita_mem_ptr(at, 4) ? vita_read32(at) : 0; }
static void wr(uint32_t at, uint32_t v) { if (vita_mem_ptr(at, 4)) vita_write32(at, v); }
static void field_set(uint32_t at, uint32_t mask, uint32_t value) { wr(at, (rd(at) & ~mask) | (value & mask)); }

static void frame_end(void);

/* --- the context's state ------------------------------------------------------------- */

static gxm_draw_state g_state;              /* raster state and bindings, as the setters leave them */

static struct {
    uint32_t object;
    uint32_t ring[2], ring_size[2], ring_at[2];     /* where default uniform buffers come from: vertex, fragment */
    uint32_t program[2];                    /* the current programs, as the title's objects */
    uint32_t precomputed[2];                /* precomputed states in force, or 0 */
    uint32_t visibility, visibility_stride;
    uint8_t  visibility_on[2], visibility_set[2];
    uint32_t visibility_index[2];
} g_ctx;

static struct {
    int      active;
    uint32_t depth_surface[5];              /* the title's depth surface, as it was at the start */
    int      has_depth_surface;
} g_scene;

static float   *g_depth;
static uint8_t *g_stencil, *g_mask;
static size_t   g_plane_cap;

static void face_defaults(gxm_face *f) {
    memset(f, 0, sizeof(*f));
    f->depth_func = 3;                      /* less or equal */
    f->depth_write = 1;
    f->stencil_func = 7;                    /* always */
    f->compare_mask = f->write_mask = 0xFF;
}

static void state_defaults(void) {
    memset(&g_state, 0, sizeof(g_state));
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_state.viewport_on = 1;
    face_defaults(&g_state.face[0]);
    face_defaults(&g_state.face[1]);
}

/* --- initialisation and memory ------------------------------------------------- */

static uint32_t g_display_cb, g_display_cb_size, g_display_cb_buf;
static uint32_t g_notification_region;

/* int sceGxmInitialize(const SceGxmInitializeParams *params)
 * { flags; displayQueueMaxPendingCount; displayQueueCallback;
 *   displayQueueCallbackDataSize; parameterBufferSize } */
void vita_hle_sceGxmInitialize(void) {
    if (!vita_mem_ptr(r0, 20)) { r0 = E_INVALID_POINTER; return; }
    g_display_cb      = vita_read32(r0 + 8);
    g_display_cb_size = vita_read32(r0 + 12);
    g_display_cb_buf  = object(g_display_cb_size ? g_display_cb_size : 16);
    state_defaults();
    vita_hle_trace("sceGxmInitialize(pending %u, callback 0x%08X, data %u bytes, parameter buffer 0x%X)",
                   vita_read32(r0 + 4), g_display_cb, g_display_cb_size, vita_read32(r0 + 16));
    r0 = 0;
}
void vita_hle_sceGxmTerminate(void) { r0 = 0; }

/* int sceGxmMapMemory(void *base, SceSize size, SceGxmMemoryAttribFlags attr): the renderer sees all memory */
void vita_hle_sceGxmMapMemory(void)   { vita_hle_trace("sceGxmMapMemory(0x%08X, 0x%X, %u)", r0, r1, r2); r0 = 0; }
void vita_hle_sceGxmUnmapMemory(void) { r0 = 0; }

/* int sceGxmMapVertexUsseMemory(void *base, SceSize size, unsigned int *offset) */
void vita_hle_sceGxmMapVertexUsseMemory(void)     { wr(r2, 0); r0 = 0; }
void vita_hle_sceGxmMapFragmentUsseMemory(void)   { wr(r2, 0); r0 = 0; }
void vita_hle_sceGxmUnmapVertexUsseMemory(void)   { r0 = 0; }
void vita_hle_sceGxmUnmapFragmentUsseMemory(void) { r0 = 0; }

/* volatile unsigned int *sceGxmGetNotificationRegion(void): 512 words the
 * title and the GPU signal each other through. */
void vita_hle_sceGxmGetNotificationRegion(void) {
    if (!g_notification_region) g_notification_region = object(0x1000);
    r0 = g_notification_region;
}

/* A notification is { address; value }: the value is written when the work
 * it was attached to is done, which here is always already. */
static void notify(uint32_t n) {
    if (n && vita_mem_ptr(n, 8)) {
        uint32_t addr = vita_read32(n), value = vita_read32(n + 4);
        if (addr) wr(addr, value);
    }
}

/* int sceGxmNotificationWait(const SceGxmNotification *notification) */
void vita_hle_sceGxmNotificationWait(void) { notify(r0); r0 = 0; }

/* --- contexts, sync objects, render targets ------------------------------------- */

/* int sceGxmCreateContext(const SceGxmContextParams *params, SceGxmContext **context)
 * { hostMem; hostMemSize; vdmRingBufferMem; vdmRingBufferMemSize; vertexRingBufferMem;
 *   vertexRingBufferMemSize; fragmentRingBufferMem; fragmentRingBufferMemSize;
 *   fragmentUsseRingBufferMem; fragmentUsseRingBufferMemSize; fragmentUsseRingBufferOffset }
 * The context lives in the host memory the title provides, as on the console. */
void vita_hle_sceGxmCreateContext(void) {
    if (!vita_mem_ptr(r0, 44) || !vita_mem_ptr(r1, 4)) { r0 = E_INVALID_POINTER; return; }
    uint32_t ctx = vita_read32(r0);
    if (!ctx) ctx = object(0x100);
    state_defaults();
    g_ctx.object = ctx;
    g_ctx.ring[0] = vita_read32(r0 + 16); g_ctx.ring_size[0] = vita_read32(r0 + 20);
    g_ctx.ring[1] = vita_read32(r0 + 24); g_ctx.ring_size[1] = vita_read32(r0 + 28);
    vita_write32(r1, ctx);
    vita_hle_trace("sceGxmCreateContext(host 0x%08X, vertex ring 0x%08X+0x%X, fragment ring 0x%08X+0x%X) -> 0x%08X",
                   vita_read32(r0), g_ctx.ring[0], g_ctx.ring_size[0], g_ctx.ring[1], g_ctx.ring_size[1], ctx);
    r0 = 0;
}
void vita_hle_sceGxmDestroyContext(void) { r0 = 0; }

/* int sceGxmSyncObjectCreate(SceGxmSyncObject **syncObject) */
void vita_hle_sceGxmSyncObjectCreate(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = E_INVALID_POINTER; return; }
    vita_write32(r0, object(16));
    r0 = 0;
}
void vita_hle_sceGxmSyncObjectDestroy(void) { r0 = 0; }

/* int sceGxmGetRenderTargetMemSize(const SceGxmRenderTargetParams *params, unsigned int *driverMemSize) */
void vita_hle_sceGxmGetRenderTargetMemSize(void) { wr(r1, 0x10000); r0 = 0; }

/* int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget)
 * { flags; u16 width; u16 height; u16 scenesPerFrame; u16 multisampleMode; multisampleLocations;
 *   driverMemBlock }: the record is a copy of those 20 bytes. */
void vita_hle_sceGxmCreateRenderTarget(void) {
    if (!vita_mem_ptr(r0, 20) || !vita_mem_ptr(r1, 4)) { r0 = E_INVALID_POINTER; return; }
    uint32_t rt = object(32);
    if (!rt) { r0 = E_OUT_OF_MEMORY; return; }
    for (uint32_t k = 0; k < 20; k += 4) vita_write32(rt + k, vita_read32(r0 + k));
    vita_write32(r1, rt);
    vita_hle_trace("sceGxmCreateRenderTarget(%ux%u, multisample %u) -> 0x%08X", vita_read16(r0 + 4), vita_read16(r0 + 6),
                   vita_read16(r0 + 10), rt);
    r0 = 0;
}
void vita_hle_sceGxmDestroyRenderTarget(void) { r0 = 0; }

/* int sceGxmRenderTargetGetDriverMemBlock(const SceGxmRenderTarget *renderTarget, SceUID *driverMemBlock) */
void vita_hle_sceGxmRenderTargetGetDriverMemBlock(void) {
    if (vita_mem_ptr(r0, 20) && vita_mem_ptr(r1, 4)) vita_write32(r1, vita_read32(r0 + 16));
    r0 = 0;
}

/* --- textures: the hardware's control words --------------------------------------------
 * Word 0: -:1 stride_ext:2 vaddr:3 uaddr:3 mip_filter:1 min:2 mag:2 -:3
 * mip_count:4 lod_bias:6 gamma:2 -:2 format0:1. Word 1: height-1:12
 * width-1:12 (or the two log2 sizes, for the kinds whose sides are powers of
 * two) base_format:5 type:3. Word 2: lod_min:2 data>>2:30. Word 3:
 * palette>>6:26 lod_min:2 swizzle:3 normalize:1. A format value is
 * base_format << 24 | format0 << 31 | swizzle << 12. A strided texture has
 * its row length where the mip settings are in the others. */

static uint32_t log2_u(uint32_t v) { uint32_t n = 0; while (v > 1) { v >>= 1; n++; } return n; }
static int tex_type(uint32_t t) { return (int)(rd(t + 4) >> 29); }

static void texture_format_set(uint32_t t, uint32_t format) {
    field_set(t + 0, 0x80000000u, format);
    field_set(t + 4, 0x1F000000u, format);
    field_set(t + 12, 7u << 28, ((format >> 12) & 7) << 28);
}

static void texture_stride_set(uint32_t t, uint32_t bytes) {
    uint32_t s = (bytes >> 2) - 1;
    field_set(t, (7u << 9) | (0x3FFu << 17) | (3u << 1), ((s & 7) << 9) | (((s >> 3) & 0x3FF) << 17) | (((s >> 13) & 3) << 1));
}

static uint32_t texture_init(uint32_t t, uint32_t data, uint32_t format, uint32_t width, uint32_t height,
                             uint32_t mips, uint32_t type, uint32_t stride) {
    if (!vita_mem_ptr(t, 16)) return E_INVALID_POINTER;
    if (!width || !height || width > 4096 || height > 4096 || (type != GXM_TEX_LINEAR_STRIDED && mips > 13)) return E_INVALID_VALUE;
    int power_of_two = type == GXM_TEX_SWIZZLED || type == GXM_TEX_CUBE;
    if (power_of_two && ((width & (width - 1)) || (height & (height - 1)))) return E_INVALID_ALIGN;
    /* clamped at the edges, the nearest texel, coordinates from 0 to 1, no bias */
    uint32_t w0 = (2u << 6) | (2u << 3);
    if (type != GXM_TEX_LINEAR_STRIDED) {
        uint32_t count = mips - 1;                          /* none at all is the marker 15 */
        w0 |= ((count > 15 ? 15 : count) & 15) << 17;
        w0 |= 31u << 21;
    }
    uint32_t w1 = power_of_two ? (log2_u(height) & 0xF) | ((log2_u(width) & 0xF) << 16)
                               : ((height - 1) & 0xFFF) | (((width - 1) & 0xFFF) << 12);
    vita_write32(t + 0, w0);
    vita_write32(t + 4, w1 | (type << 29));
    vita_write32(t + 8, data & ~3u);
    vita_write32(t + 12, 0x80000000u);
    texture_format_set(t, format);
    if (type == GXM_TEX_LINEAR_STRIDED) texture_stride_set(t, stride);
    return 0;
}

/* int sceGxmTextureInitLinear(texture, data, texFormat, width, height, mipCount): two on the stack */
void vita_hle_sceGxmTextureInitLinear(void)   { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), GXM_TEX_LINEAR, 0); }
void vita_hle_sceGxmTextureInitSwizzled(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), GXM_TEX_SWIZZLED, 0); }
void vita_hle_sceGxmTextureInitSwizzledArbitrary(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), GXM_TEX_SWIZZLED_ARBITRARY, 0); }
void vita_hle_sceGxmTextureInitTiled(void)    { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), GXM_TEX_TILED, 0); }
void vita_hle_sceGxmTextureInitCube(void)     { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), GXM_TEX_CUBE, 0); }
void vita_hle_sceGxmTextureInitCubeArbitrary(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), GXM_TEX_CUBE_ARBITRARY, 0); }
/* int sceGxmTextureInitLinearStrided(texture, data, texFormat, width, height, byteStride) */
void vita_hle_sceGxmTextureInitLinearStrided(void) {
    uint32_t stride = arg(5);
    if (stride & 3) { r0 = E_INVALID_ALIGN; return; }
    if (stride < 4 || stride > 131072) { r0 = E_INVALID_VALUE; return; }
    r0 = texture_init(r0, r1, r2, r3, arg(4), 1, GXM_TEX_LINEAR_STRIDED, stride);
}

static uint32_t texture_format(uint32_t t) {
    return (rd(t + 4) & 0x1F000000u) | (rd(t) & 0x80000000u) | (((rd(t + 12) >> 28) & 7) << 12);
}
static int tex_sides_are_logs(uint32_t t) { return tex_type(t) == GXM_TEX_SWIZZLED || tex_type(t) == GXM_TEX_CUBE; }

void vita_hle_sceGxmTextureGetType(void)   { r0 = rd(r0 + 4) & 0xE0000000u; }
void vita_hle_sceGxmTextureGetFormat(void) { r0 = texture_format(r0); }
void vita_hle_sceGxmTextureSetFormat(void) { texture_format_set(r0, r1); r0 = 0; }
void vita_hle_sceGxmTextureGetWidth(void) {
    uint32_t w1 = rd(r0 + 4);
    r0 = tex_sides_are_logs(r0) ? 1u << ((w1 >> 16) & 0xF) : ((w1 >> 12) & 0xFFF) + 1;
}
void vita_hle_sceGxmTextureGetHeight(void) {
    uint32_t w1 = rd(r0 + 4);
    r0 = tex_sides_are_logs(r0) ? 1u << (w1 & 0xF) : (w1 & 0xFFF) + 1;
}
void vita_hle_sceGxmTextureSetWidth(void) {
    if (!r1 || r1 > 4096) { r0 = E_INVALID_VALUE; return; }
    if (tex_sides_are_logs(r0)) field_set(r0 + 4, 0xFu << 16, log2_u(r1) << 16);
    else field_set(r0 + 4, 0xFFFu << 12, (r1 - 1) << 12);
    r0 = 0;
}
void vita_hle_sceGxmTextureSetHeight(void) {
    if (!r1 || r1 > 4096) { r0 = E_INVALID_VALUE; return; }
    if (tex_sides_are_logs(r0)) field_set(r0 + 4, 0xFu, log2_u(r1));
    else field_set(r0 + 4, 0xFFFu, r1 - 1);
    r0 = 0;
}
void vita_hle_sceGxmTextureGetData(void)    { r0 = rd(r0 + 8) & ~3u; }
void vita_hle_sceGxmTextureSetData(void)    { field_set(r0 + 8, ~3u, r1); r0 = 0; }
void vita_hle_sceGxmTextureGetPalette(void) {
    uint32_t base = texture_format(r0) & 0x9F000000u;
    r0 = (base == 0x94000000u || base == 0x95000000u) ? (rd(r0 + 12) & 0x03FFFFFFu) << 6 : 0;
}
void vita_hle_sceGxmTextureSetPalette(void) {
    if (r1 & 63) { r0 = E_INVALID_ALIGN; return; }
    field_set(r0 + 12, 0x03FFFFFFu, r1 >> 6);
    r0 = 0;
}
static uint32_t tex_mip_count(uint32_t t) { return tex_type(t) == GXM_TEX_LINEAR_STRIDED ? 0 : (((rd(t) >> 17) & 0xF) + 1) & 0xF; }
void vita_hle_sceGxmTextureGetMipmapCount(void)       { r0 = tex_mip_count(r0); }
void vita_hle_sceGxmTextureGetMipmapCountUnsafe(void) { r0 = tex_mip_count(r0); }
void vita_hle_sceGxmTextureSetMipmapCount(void) {
    if (tex_type(r0) == GXM_TEX_LINEAR_STRIDED) { r0 = E_UNSUPPORTED; return; }
    if (r1 > 13) { r0 = E_INVALID_VALUE; return; }
    field_set(r0, 0xFu << 17, ((r1 - 1) & 0xF) << 17);
    r0 = 0;
}
void vita_hle_sceGxmTextureGetMagFilter(void) { r0 = (rd(r0) >> 12) & 3; }
void vita_hle_sceGxmTextureSetMagFilter(void) { field_set(r0, 3u << 12, (r1 & 3) << 12); r0 = 0; }
void vita_hle_sceGxmTextureGetMinFilter(void) { r0 = tex_type(r0) == GXM_TEX_LINEAR_STRIDED ? (rd(r0) >> 12) & 3 : (rd(r0) >> 10) & 3; }
void vita_hle_sceGxmTextureSetMinFilter(void) {
    if (tex_type(r0) == GXM_TEX_LINEAR_STRIDED) { r0 = E_UNSUPPORTED; return; }
    field_set(r0, 3u << 10, (r1 & 3) << 10);
    r0 = 0;
}
/* the mip filter's values are the bit itself, in place */
void vita_hle_sceGxmTextureGetMipFilter(void) { r0 = tex_type(r0) == GXM_TEX_LINEAR_STRIDED ? 0 : rd(r0) & 0x200u; }
void vita_hle_sceGxmTextureSetMipFilter(void) {
    if (tex_type(r0) == GXM_TEX_LINEAR_STRIDED) { r0 = E_UNSUPPORTED; return; }
    field_set(r0, 0x200u, r1 ? 0x200u : 0);
    r0 = 0;
}
void vita_hle_sceGxmTextureGetUAddrMode(void) { r0 = (rd(r0) >> 6) & 7; }
void vita_hle_sceGxmTextureSetUAddrMode(void) { field_set(r0, 7u << 6, (r1 & 7) << 6); r0 = 0; }
void vita_hle_sceGxmTextureGetVAddrMode(void) { r0 = (rd(r0) >> 3) & 7; }
void vita_hle_sceGxmTextureSetVAddrMode(void) { field_set(r0, 7u << 3, (r1 & 7) << 3); r0 = 0; }
void vita_hle_sceGxmTextureGetUAddrModeSafe(void) { vita_hle_sceGxmTextureGetUAddrMode(); }
void vita_hle_sceGxmTextureSetUAddrModeSafe(void) { vita_hle_sceGxmTextureSetUAddrMode(); }
void vita_hle_sceGxmTextureGetVAddrModeSafe(void) { vita_hle_sceGxmTextureGetVAddrMode(); }
void vita_hle_sceGxmTextureSetVAddrModeSafe(void) { vita_hle_sceGxmTextureSetVAddrMode(); }
void vita_hle_sceGxmTextureGetLodBias(void)   { r0 = tex_type(r0) == GXM_TEX_LINEAR_STRIDED ? 0 : (rd(r0) >> 21) & 0x3F; }
void vita_hle_sceGxmTextureSetLodBias(void) {
    if (tex_type(r0) == GXM_TEX_LINEAR_STRIDED) { r0 = E_UNSUPPORTED; return; }
    if (r1 > 63) { r0 = E_INVALID_VALUE; return; }
    field_set(r0, 0x3Fu << 21, r1 << 21);
    r0 = 0;
}
void vita_hle_sceGxmTextureGetLodMin(void)    { r0 = (rd(r0 + 8) & 3) | (((rd(r0 + 12) >> 26) & 3) << 2); }
void vita_hle_sceGxmTextureSetLodMin(void) {
    if (tex_type(r0) == GXM_TEX_LINEAR_STRIDED) { r0 = E_UNSUPPORTED; return; }
    field_set(r0 + 8, 3u, r1 & 3);
    field_set(r0 + 12, 3u << 26, ((r1 >> 2) & 3) << 26);
    r0 = 0;
}
/* the gamma mode's values are the field, in place */
void vita_hle_sceGxmTextureGetGammaMode(void) { r0 = rd(r0) & (3u << 27); }
void vita_hle_sceGxmTextureSetGammaMode(void) { field_set(r0, 3u << 27, r1); r0 = 0; }
void vita_hle_sceGxmTextureGetStride(void) {
    uint32_t w0 = rd(r0);
    r0 = tex_type(r0) == GXM_TEX_LINEAR_STRIDED
       ? ((((w0 >> 9) & 7) | (((w0 >> 17) & 0x3FF) << 3) | (((w0 >> 1) & 3) << 13)) + 1) * 4 : 0;
}
void vita_hle_sceGxmTextureSetStride(void) {
    if (tex_type(r0) != GXM_TEX_LINEAR_STRIDED) { r0 = E_UNSUPPORTED; return; }
    if (r1 & 3) { r0 = E_INVALID_ALIGN; return; }
    if (r1 < 4 || r1 > 131072) { r0 = E_INVALID_VALUE; return; }
    texture_stride_set(r0, r1);
    r0 = 0;
}
void vita_hle_sceGxmTextureGetNormalizeMode(void) { r0 = rd(r0 + 12) & 0x80000000u; }
void vita_hle_sceGxmTextureSetNormalizeMode(void) { field_set(r0 + 12, 0x80000000u, r1 ? 0x80000000u : 0); r0 = 0; }
void vita_hle_sceGxmTextureValidate(void) { r0 = 0; }

/* --- colour surfaces --------------------------------------------------------------------
 * SceGxmColorSurface is 0x30 bytes of the title's. The SDK fixes two things
 * in it, the output register size at 0x1C and a texture over the same
 * pixels at 0x20; the rest is this runtime's:
 *
 *   0x00  bit 0 enabled, bit 1 downscale, bits 4..5 gamma, bit 6 dither,
 *         bits 8..19 clip xMin, bits 20..31 clip yMin
 *   0x04  width | height << 16          0x08  stride in pixels
 *   0x0C  data                          0x10  colour format
 *   0x14  surface type                  0x18  clip xMax | yMax << 12 */

enum { CS_FLAGS = 0, CS_SIZE = 4, CS_STRIDE = 8, CS_DATA = 12, CS_FORMAT = 16, CS_TYPE = 20, CS_CLIP = 24,
       CS_REGISTER = 28, CS_TEXTURE = 32, CS_BYTES = 48 };

/* The texture a colour surface is when it is read instead of written. */
static void background_texture(uint32_t s) {
    uint32_t format = pixfmt_colour_texture(rd(s + CS_FORMAT)), type = rd(s + CS_TYPE);
    uint32_t width = rd(s + CS_SIZE) & 0xFFFF, height = rd(s + CS_SIZE) >> 16, stride = rd(s + CS_STRIDE);
    pixfmt f;
    if (!pixfmt_colour(rd(s + CS_FORMAT), &f)) return;
    if (type == 0x08000000u)      texture_init(s + CS_TEXTURE, rd(s + CS_DATA), format, width, height, 1, GXM_TEX_SWIZZLED, 0);
    else if (type == 0x04000000u) texture_init(s + CS_TEXTURE, rd(s + CS_DATA), format, width, height, 1, GXM_TEX_TILED, 0);
    else if (stride == ((width + 7) & ~7u))
        texture_init(s + CS_TEXTURE, rd(s + CS_DATA), format, width, height, 1, GXM_TEX_LINEAR, 0);
    else
        texture_init(s + CS_TEXTURE, rd(s + CS_DATA), format, width, height, 1, GXM_TEX_LINEAR_STRIDED, stride * f.bytes);
}

/* int sceGxmColorSurfaceInit(surface, colorFormat, surfaceType, scaleMode, outputRegisterSize,
 *                            width, height, strideInPixels, data): five arguments on the stack */
void vita_hle_sceGxmColorSurfaceInit(void) {
    uint32_t s = r0, format = r1, type = r2, scale = r3;
    uint32_t reg = arg(4), width = arg(5), height = arg(6), stride = arg(7), data = arg(8);
    if (!vita_mem_ptr(s, CS_BYTES) || !data) { r0 = E_INVALID_POINTER; return; }
    if (!width || !height || width > 4096 || height > 4096 || stride < width || (data & 3)) { r0 = E_INVALID_VALUE; return; }
    if (stride & 1) { r0 = E_INVALID_ALIGN; return; }
    memset(vita_mem_ptr(s, CS_BYTES), 0, CS_BYTES);
    vita_write32(s + CS_FLAGS, 1u | ((scale & 1) << 1));
    vita_write32(s + CS_SIZE, width | (height << 16));
    vita_write32(s + CS_STRIDE, stride);
    vita_write32(s + CS_DATA, data);
    vita_write32(s + CS_FORMAT, format);
    vita_write32(s + CS_TYPE, type);
    vita_write32(s + CS_CLIP, ((width - 1) & 0xFFF) | (((height - 1) & 0xFFF) << 12));
    vita_write32(s + CS_REGISTER, reg);
    background_texture(s);
    vita_hle_trace("sceGxmColorSurfaceInit(0x%08X, format 0x%08X, type 0x%X, %ux%u stride %u, data 0x%08X)",
                   s, format, type, width, height, stride, data);
    r0 = 0;
}
void vita_hle_sceGxmColorSurfaceInitDisabled(void) {
    void *p = vita_mem_ptr(r0, CS_BYTES);
    if (!p) { r0 = E_INVALID_POINTER; return; }
    memset(p, 0, CS_BYTES);
    r0 = 0;
}
void vita_hle_sceGxmColorSurfaceIsEnabled(void)   { r0 = rd(r0 + CS_FLAGS) & 1; }
void vita_hle_sceGxmColorSurfaceGetData(void)     { r0 = rd(r0 + CS_DATA); }
void vita_hle_sceGxmColorSurfaceSetData(void) {
    if (r1 & 3) { r0 = E_INVALID_VALUE; return; }
    wr(r0 + CS_DATA, r1);
    field_set(r0 + CS_TEXTURE + 8, ~3u, r1);
    r0 = 0;
}
void vita_hle_sceGxmColorSurfaceGetFormat(void)   { r0 = rd(r0 + CS_FORMAT); }
void vita_hle_sceGxmColorSurfaceSetFormat(void)   { uint32_t s = r0; wr(s + CS_FORMAT, r1); background_texture(s); r0 = 0; }
void vita_hle_sceGxmColorSurfaceGetType(void)     { r0 = rd(r0 + CS_TYPE); }
void vita_hle_sceGxmColorSurfaceGetStrideInPixels(void) { r0 = rd(r0 + CS_STRIDE); }
void vita_hle_sceGxmColorSurfaceGetScaleMode(void) { r0 = (rd(r0 + CS_FLAGS) >> 1) & 1; }
void vita_hle_sceGxmColorSurfaceSetScaleMode(void) { field_set(r0 + CS_FLAGS, 2u, (r1 & 1) << 1); }
/* gamma modes: none 0, the colour channels 0x1000, the two channels of a two-channel format 0x3000 */
void vita_hle_sceGxmColorSurfaceGetGammaMode(void) { r0 = ((rd(r0 + CS_FLAGS) >> 4) & 3) << 12; }
void vita_hle_sceGxmColorSurfaceSetGammaMode(void) {
    field_set(r0 + CS_FLAGS, 3u << 4, ((r1 >> 12) & 3) << 4);
    field_set(r0 + CS_TEXTURE, 3u << 27, ((r1 >> 12) & 3) << 27);
    r0 = 0;
}
void vita_hle_sceGxmColorSurfaceGetDitherMode(void) { r0 = (rd(r0 + CS_FLAGS) & 0x40u) ? 8 : 0; }
void vita_hle_sceGxmColorSurfaceSetDitherMode(void) { field_set(r0 + CS_FLAGS, 0x40u, r1 ? 0x40u : 0); r0 = 0; }
/* void sceGxmColorSurfaceSetClip(surface, xMin, yMin, xMax, yMax) */
void vita_hle_sceGxmColorSurfaceSetClip(void) {
    field_set(r0 + CS_FLAGS, 0xFFFFFF00u, ((r1 & 0xFFF) << 8) | ((r2 & 0xFFF) << 20));
    wr(r0 + CS_CLIP, (r3 & 0xFFF) | ((arg(4) & 0xFFF) << 12));
}
/* void sceGxmColorSurfaceGetClip(surface, *xMin, *yMin, *xMax, *yMax) */
void vita_hle_sceGxmColorSurfaceGetClip(void) {
    uint32_t flags = rd(r0 + CS_FLAGS), clip = rd(r0 + CS_CLIP);
    wr(r1, (flags >> 8) & 0xFFF); wr(r2, flags >> 20); wr(r3, clip & 0xFFF); wr(arg(4), (clip >> 12) & 0xFFF);
}

/* --- depth and stencil surfaces ----------------------------------------------------------
 * SceGxmDepthStencilSurface is 0x14 bytes: a control word (bit 0 enabled,
 * bit 1 load at the start of a scene, bit 2 store at its end, bits 3..10
 * the row length in 32-sample tiles less one, above them the format and
 * the layout in place), the depth and the stencil memory, the background
 * depth, and a word with the background stencil in its low byte and the
 * background mask in bit 8. */

enum { DS_CONTROL = 0, DS_DEPTH = 4, DS_STENCIL = 8, DS_BACKGROUND = 12, DS_BACKGROUND2 = 16, DS_BYTES = 20 };
#define DS_FORMAT_BITS 0x07EEE000u
#define DS_TILED       0x00011000u

/* int sceGxmDepthStencilSurfaceInit(surface, depthStencilFormat, surfaceType, strideInSamples,
 *                                   void *depthData, void *stencilData): two on the stack */
void vita_hle_sceGxmDepthStencilSurfaceInit(void) {
    uint32_t s = r0, format = r1, type = r2, stride = r3;
    if (!vita_mem_ptr(s, DS_BYTES)) { r0 = E_INVALID_POINTER; return; }
    if (!stride || (stride & 31)) { r0 = E_INVALID_VALUE; return; }
    vita_write32(s + DS_CONTROL, 1u | (((stride / 32 - 1) & 0xFF) << 3) | format | type | 0x00100000u);
    vita_write32(s + DS_DEPTH, arg(4));
    vita_write32(s + DS_STENCIL, arg(5));
    vita_write32(s + DS_BACKGROUND, 0x3F800000u);
    vita_write32(s + DS_BACKGROUND2, 0x300u);
    vita_hle_trace("sceGxmDepthStencilSurfaceInit(0x%08X, format 0x%X, type 0x%X, stride %u, depth 0x%08X, stencil 0x%08X)",
                   s, format, type, stride, arg(4), arg(5));
    r0 = 0;
}
void vita_hle_sceGxmDepthStencilSurfaceInitDisabled(void) {
    if (!vita_mem_ptr(r0, DS_BYTES)) { r0 = E_INVALID_POINTER; return; }
    vita_write32(r0 + DS_CONTROL, 0); vita_write32(r0 + DS_DEPTH, 0); vita_write32(r0 + DS_STENCIL, 0);
    vita_write32(r0 + DS_BACKGROUND, 0x3F800000u); vita_write32(r0 + DS_BACKGROUND2, 0x300u);
    r0 = 0;
}
void vita_hle_sceGxmDepthStencilSurfaceIsEnabled(void) { r0 = (rd(r0 + DS_CONTROL) & DS_FORMAT_BITS) != 0; }
void vita_hle_sceGxmDepthStencilSurfaceGetFormat(void) { r0 = rd(r0 + DS_CONTROL) & DS_FORMAT_BITS; }
void vita_hle_sceGxmDepthStencilSurfaceGetStrideInSamples(void) { r0 = (((rd(r0 + DS_CONTROL) >> 3) & 0xFF) + 1) * 32; }
/* the two force modes' values are their bits, in place: 2 and 4 */
void vita_hle_sceGxmDepthStencilSurfaceGetForceLoadMode(void)  { r0 = rd(r0 + DS_CONTROL) & 2u; }
void vita_hle_sceGxmDepthStencilSurfaceSetForceLoadMode(void)  { field_set(r0 + DS_CONTROL, 2u, r1); }
void vita_hle_sceGxmDepthStencilSurfaceGetForceStoreMode(void) { r0 = rd(r0 + DS_CONTROL) & 4u; }
void vita_hle_sceGxmDepthStencilSurfaceSetForceStoreMode(void) { field_set(r0 + DS_CONTROL, 4u, r1); }
/* the background depth is a float, passed and returned in s0 */
void vita_hle_sceGxmDepthStencilSurfaceGetBackgroundDepth(void) { uint32_t u = rd(r0 + DS_BACKGROUND); float f; memcpy(&f, &u, 4); vfp_setf(0, f); }
void vita_hle_sceGxmDepthStencilSurfaceSetBackgroundDepth(void) { float f = vfp_getf(0); uint32_t u; memcpy(&u, &f, 4); wr(r0 + DS_BACKGROUND, u); }
void vita_hle_sceGxmDepthStencilSurfaceGetBackgroundStencil(void) { r0 = rd(r0 + DS_BACKGROUND2) & 0xFF; }
void vita_hle_sceGxmDepthStencilSurfaceSetBackgroundStencil(void) { field_set(r0 + DS_BACKGROUND2, 0xFFu, r1); }
void vita_hle_sceGxmDepthStencilSurfaceGetBackgroundMask(void)    { r0 = (rd(r0 + DS_BACKGROUND2) >> 8) & 1; }
void vita_hle_sceGxmDepthStencilSurfaceSetBackgroundMask(void)    { field_set(r0 + DS_BACKGROUND2, 0x100u, r1 ? 0x100u : 0); }

/* --- the display queue --------------------------------------------------------------
 * The driver copies the title's callback data and, when the frame is done,
 * calls the callback with the copy. The frame is done at once here. */

/* int sceGxmDisplayQueueAddEntry(SceGxmSyncObject *oldBuffer, SceGxmSyncObject *newBuffer, const void *callbackData) */
void vita_hle_sceGxmDisplayQueueAddEntry(void) {
    uint32_t data = r2;
    frame_end();
    if (g_display_cb && g_display_cb_buf && g_display_cb_size) {
        void *dst = vita_mem_ptr(g_display_cb_buf, g_display_cb_size);
        const void *src = vita_mem_ptr(data, g_display_cb_size);
        if (dst && src) memcpy(dst, src, g_display_cb_size);
        uint32_t s0 = r0, s1 = r1, s2 = r2, s3 = r3;
        r0 = g_display_cb_buf;
        vita_dispatch(0, g_display_cb);
        r0 = s0; r1 = s1; r2 = s2; r3 = s3;
    }
    r0 = 0;
}
void vita_hle_sceGxmDisplayQueueFinish(void) { r0 = 0; }

/* int sceGxmPadHeartbeat(const SceGxmColorSurface *displaySurface, SceGxmSyncObject *displaySyncObject) */
void vita_hle_sceGxmPadHeartbeat(void) { r0 = 0; }
void vita_hle_sceGxmPadTriggerGpuPaTrace(void) { r0 = 0; }

/* --- programs: the GXP, read where the title keeps it --------------------------------
 * Offsets inside a GXP are relative to the field that holds them. The
 * parameter table is 16 bytes a parameter: { int32 name_offset; u8
 * category:4 | type:4; u8 component_count:4 | container_index:4; u8
 * semantic; u8 semantic_index; u32 array_size; u32 resource_index }. */

#define GXP_H_FLAGS         0x14
#define GXP_H_PARAMETERS_N  0x24
#define GXP_H_PARAMETERS    0x28
#define GXP_H_DEFAULT_WORDS 0x64

static uint32_t gxp_parameters(uint32_t program) {
    if (!vita_mem_ptr(program, 0x98)) return 0;
    return program + GXP_H_PARAMETERS + vita_read32(program + GXP_H_PARAMETERS);
}
static uint32_t gxp_flags(uint32_t program) { return vita_mem_ptr(program, 0x98) ? vita_read32(program + GXP_H_FLAGS) : 0; }

/* unsigned int sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram *program): bytes */
void vita_hle_sceGxmProgramGetDefaultUniformBufferSize(void) {
    r0 = vita_mem_ptr(r0, 0x98) ? vita_read32(r0 + GXP_H_DEFAULT_WORDS) * 4u : 0;
}
void vita_hle_sceGxmProgramGetParameterCount(void) { r0 = vita_mem_ptr(r0, 0x98) ? vita_read32(r0 + GXP_H_PARAMETERS_N) : 0; }

/* const SceGxmProgramParameter *sceGxmProgramGetParameter(const SceGxmProgram *program, unsigned int index) */
void vita_hle_sceGxmProgramGetParameter(void) {
    uint32_t table = gxp_parameters(r0);
    r0 = (table && r1 < vita_read32(r0 + GXP_H_PARAMETERS_N)) ? table + r1 * 16 : 0;
}

/* const SceGxmProgramParameter *sceGxmProgramFindParameterByName(const SceGxmProgram *program, const char *name) */
void vita_hle_sceGxmProgramFindParameterByName(void) {
    uint32_t table = gxp_parameters(r0), count = table ? vita_read32(r0 + GXP_H_PARAMETERS_N) : 0;
    uint32_t avail = vita_mem_avail(r1);
    const char *want = avail ? (const char *)vita_mem_ptr(r1, 1) : NULL;
    r0 = 0;
    if (!want) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = table + i * 16;
        uint32_t name = p + (uint32_t)(int32_t)vita_read32(p);
        uint32_t n = vita_mem_avail(name);
        const char *s = n ? (const char *)vita_mem_ptr(name, 1) : NULL;
        if (s && strncmp(s, want, n < avail ? n : avail) == 0) { r0 = p; return; }
    }
}

/* const SceGxmProgramParameter *sceGxmProgramFindParameterBySemantic(program, semantic, index) */
void vita_hle_sceGxmProgramFindParameterBySemantic(void) {
    uint32_t table = gxp_parameters(r0), count = table ? vita_read32(r0 + GXP_H_PARAMETERS_N) : 0, semantic = r1, index = r2;
    r0 = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = table + i * 16;
        if ((vita_read8(p + 4) & 0xF) == 0 && vita_read8(p + 6) == semantic && (vita_read8(p + 7) & 0xF) == index) { r0 = p; return; }
    }
}

/* const char *sceGxmProgramParameterGetName(const SceGxmProgramParameter *parameter) */
void vita_hle_sceGxmProgramParameterGetName(void) { r0 = vita_mem_ptr(r0, 16) ? r0 + (uint32_t)(int32_t)vita_read32(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetCategory(void)       { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 4) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetType(void)           { r0 = vita_mem_ptr(r0, 16) ? (vita_read8(r0 + 4) >> 4) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetComponentCount(void) { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 5) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetContainerIndex(void) { r0 = vita_mem_ptr(r0, 16) ? (vita_read8(r0 + 5) >> 4) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetSemantic(void)       { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 6) : 0; }
void vita_hle_sceGxmProgramParameterGetSemanticIndex(void)  { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 7) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetArraySize(void)      { r0 = vita_mem_ptr(r0, 16) ? vita_read32(r0 + 8) : 0; }
void vita_hle_sceGxmProgramParameterGetResourceIndex(void)  { r0 = vita_mem_ptr(r0, 16) ? vita_read32(r0 + 12) : 0; }
void vita_hle_sceGxmProgramParameterIsSamplerCube(void)     { r0 = vita_mem_ptr(r0, 16) ? (vita_read8(r0 + 7) >> 4) & 1 : 0; }
void vita_hle_sceGxmProgramCheck(void)             { r0 = (vita_mem_ptr(r0, 4) && vita_read32(r0) == 0x00505847u) ? 0 : E_INVALID_VALUE; }
void vita_hle_sceGxmProgramGetSize(void)           { r0 = vita_mem_ptr(r0, 0x98) ? vita_read32(r0 + 8) : 0; }
void vita_hle_sceGxmProgramGetType(void)           { r0 = gxp_flags(r0) & 1; }
void vita_hle_sceGxmProgramIsDiscardUsed(void)     { r0 = (gxp_flags(r0) >> 3) & 1; }
void vita_hle_sceGxmProgramIsDepthReplaceUsed(void){ r0 = (gxp_flags(r0) >> 4) & 1; }
void vita_hle_sceGxmProgramIsSpriteCoordUsed(void) { r0 = (gxp_flags(r0) >> 5) & 1; }
void vita_hle_sceGxmProgramIsNativeColorUsed(void) { r0 = (gxp_flags(r0) >> 6) & 1; }
void vita_hle_sceGxmProgramIsFragColorUsed(void)   { r0 = (gxp_flags(r0) >> 7) & 1; }
void vita_hle_sceGxmProgramIsEquivalent(void)      { r0 = r0 == r1; }
void vita_hle_sceGxmProgramGetVertexProgramOutputs(void) { r0 = 0; }
void vita_hle_sceGxmProgramGetFragmentProgramInputs(void) { r0 = 0; }
void vita_hle_sceGxmProgramGetOutputRegisterFormat(void)  { r0 = 0; }

/* int sceGxmSetUniformDataF(void *uniformBuffer, const SceGxmProgramParameter *parameter,
 *                           unsigned int componentOffset, unsigned int componentCount, const float *sourceData)
 * Each float goes where the parameter's type and shape put it: scalars and
 * the elements of scalar arrays a word apart; the vectors of an array each
 * starting on a boundary of eight bytes, or four for integers. */
void vita_hle_sceGxmSetUniformDataF(void) {
    uint32_t buf = r0, param = r1, off = r2, count = r3, src = arg(4);
    if (!buf || !src || !vita_mem_ptr(param, 16)) { r0 = E_INVALID_POINTER; return; }
    uint32_t category = vita_read8(param + 4) & 0xF, type = vita_read8(param + 4) >> 4;
    uint32_t components = vita_read8(param + 5) & 0xF, array = vita_read32(param + 8);
    if (category != 1) { r0 = E_INVALID_VALUE; return; }
    if (!components) components = 1;
    uint32_t base = buf + vita_read32(param + 12) * 4u;
    uint32_t size = (type == GXP_F16 || type == GXP_U16 || type == GXP_S16) ? 2 : (type == GXP_U8 || type == GXP_S8) ? 1 : 4;
    int floating = type == GXP_F32 || type == GXP_F16 || type == GXP_C10;
    uint32_t stride = components * size;
    stride = floating ? (stride + 7) & ~7u : (stride + 3) & ~3u;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t c = off + k, at, bits = rd(src + 4 * k);
        float f;
        memcpy(&f, &bits, 4);
        if (array == 1 || components == 1) at = base + c * (components == 1 ? 4 : size);
        else at = base + (c / components) * stride + (c % components) * size;
        if (!vita_mem_ptr(at, size)) continue;
        if (type == GXP_F16)      vita_write16(at, vita_f32_to_f16(f));
        else if (size == 2)       vita_write16(at, (uint16_t)(int32_t)f);
        else if (size == 1)       vita_write8(at, (uint8_t)(int32_t)f);
        else if (floating)        vita_write32(at, bits);
        else                      vita_write32(at, type == GXP_U32 ? (uint32_t)(f < 0 ? 0 : f) : (uint32_t)(int32_t)f);
    }
    r0 = 0;
}

/* --- the shader patcher ----------------------------------------------------------------
 * A program id is the GXP header itself. A vertex or fragment program is a
 * small object the title passes back: { the header, a number, a mark, a
 * reference count }; the number finds the record kept here, which holds the
 * program read and decoded and what was fixed when it was created. Asked
 * twice for the same combination, the patcher gives the same object. */

#define PROGRAM_MARK 0x6D617247u
enum { PROGRAM_VERTEX = 0, PROGRAM_FRAGMENT, PROGRAM_MASK };

static gxm_program **g_programs;
static uint32_t *g_program_object;
static uint8_t  *g_program_kind;
static uint32_t  g_program_count, g_program_cap;

static const gxm_program *program_of(uint32_t obj) {
    if (!obj || !vita_mem_ptr(obj, 16) || vita_read32(obj + 8) != PROGRAM_MARK) return NULL;
    uint32_t n = vita_read32(obj + 4);
    return n && n <= g_program_count ? g_programs[n - 1] : NULL;
}

/* int sceGxmShaderPatcherCreate(const SceGxmShaderPatcherParams *params, SceGxmShaderPatcher **shaderPatcher) */
void vita_hle_sceGxmShaderPatcherCreate(void) {
    if (!vita_mem_ptr(r1, 4)) { r0 = E_INVALID_POINTER; return; }
    vita_write32(r1, object(64));
    vita_hle_trace("sceGxmShaderPatcherCreate -> 0x%08X", vita_read32(r1));
    r0 = 0;
}
void vita_hle_sceGxmShaderPatcherDestroy(void) { r0 = 0; }

static uint64_t program_name(const uint8_t *bytes, uint32_t size) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

/* VITARECOMP_GXP_DUMP=<directory>: each program a title registers is written
 * there once, named by a hash of its bytes, so the shaders a title actually
 * uses can be read, disassembled and given to another translator to compare. */
static void dump_program(uint32_t header) {
    static const char *dir;
    static int asked;
    if (!asked) { asked = 1; dir = getenv("VITARECOMP_GXP_DUMP"); }
    if (!dir || !*dir || !vita_mem_ptr(header, 0x98) || vita_read32(header) != 0x00505847u) return;
    uint32_t size = vita_read32(header + 8);
    const uint8_t *bytes = size >= 0x98 && size < 0x100000 ? vita_mem_ptr(header, size) : NULL;
    if (!bytes) return;
    uint64_t h = program_name(bytes, size);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_%016llx.gxp", dir, (vita_read32(header + GXP_H_FLAGS) & 1) ? "f" : "v",
             (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(bytes, 1, size, f);
    fclose(f);
}

/* int sceGxmShaderPatcherRegisterProgram(patcher, const SceGxmProgram *programHeader, SceGxmShaderPatcherId *programId) */
void vita_hle_sceGxmShaderPatcherRegisterProgram(void) {
    if (!vita_mem_ptr(r2, 4)) { r0 = E_INVALID_POINTER; return; }
    dump_program(r1);
    vita_write32(r2, r1);
    r0 = 0;
}
void vita_hle_sceGxmShaderPatcherUnregisterProgram(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherForceUnregisterProgram(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetProgramFromId(void) { /* the id is the header */ }

/* Find the program made from these, or make it. */
static uint32_t program_create(int kind, const gxm_program *want, uint32_t out) {
    if (!vita_mem_ptr(out, 4)) return E_INVALID_POINTER;
    for (uint32_t i = 0; i < g_program_count; i++) {
        const gxm_program *p = g_programs[i];
        if (g_program_kind[i] != kind || p->header != want->header) continue;
        if (p->attribute_count != want->attribute_count || p->stream_count != want->stream_count ||
            memcmp(p->attributes, want->attributes, sizeof(p->attributes)) || memcmp(p->streams, want->streams, sizeof(p->streams)) ||
            memcmp(p->blend, want->blend, 4) || p->output_format != want->output_format) continue;
        field_set(g_program_object[i] + 12, 0xFFFFFFFFu, rd(g_program_object[i] + 12) + 1);
        vita_write32(out, g_program_object[i]);
        return 0;
    }
    if (g_program_count == g_program_cap) {
        uint32_t cap = g_program_cap ? g_program_cap * 2 : 256;
        gxm_program **a = realloc(g_programs, cap * sizeof(*a));
        uint32_t *b = a ? realloc(g_program_object, cap * sizeof(*b)) : NULL;
        uint8_t *c = b ? realloc(g_program_kind, cap) : NULL;
        if (a) g_programs = a;
        if (b) g_program_object = b;
        if (!c) return E_OUT_OF_MEMORY;
        g_program_kind = c; g_program_cap = cap;
    }
    gxm_program *p = malloc(sizeof(*p));
    uint32_t obj = object(16);
    if (!p || !obj) { free(p); return E_OUT_OF_MEMORY; }
    *p = *want;
    memset(&p->gxp, 0, sizeof(p->gxp));
    p->name = 0;
    p->ok = 0;
    p->wide = 0;
    if (kind != PROGRAM_MASK && vita_mem_ptr(p->header, 0x98) && vita_read32(p->header) == 0x00505847u) {
        uint32_t size = vita_read32(p->header + 8);
        const uint8_t *bytes = size >= 0x98 && size < 0x1000000 ? vita_mem_ptr(p->header, size) : NULL;
        int err = bytes ? gxp_parse(&p->gxp, bytes, size) : -100;
        if (bytes) p->name = program_name(bytes, size);
        uint32_t cannot = 0;
        for (uint32_t i = 0; !err && i < p->gxp.secondary_count; i++) cannot += !usse_can_run(&p->gxp.secondary[i]);
        for (uint32_t i = 0; !err && i < p->gxp.primary_count; i++) cannot += !usse_can_run(&p->gxp.primary[i]);
        p->ok = !err && !cannot;
        p->wide = p->ok && usse_wide_can_run(p->gxp.primary, p->gxp.primary_count);
        if (!p->ok)
            vita_hle_trace("gxm: the program at 0x%08X cannot be run (read %d, %u instructions without an implementation)",
                           p->header, err, cannot);
    }
    g_programs[g_program_count] = p;
    g_program_object[g_program_count] = obj;
    g_program_kind[g_program_count] = (uint8_t)kind;
    g_program_count++;
    vita_write32(obj, p->header);
    vita_write32(obj + 4, g_program_count);
    vita_write32(obj + 8, PROGRAM_MARK);
    vita_write32(obj + 12, 1);
    vita_write32(out, obj);
    return 0;
}

/* int sceGxmShaderPatcherCreateVertexProgram(patcher, programId, const SceGxmVertexAttribute *attributes,
 *         unsigned int attributeCount, const SceGxmVertexStream *streams, unsigned int streamCount,
 *         SceGxmVertexProgram **vertexProgram): three on the stack */
void vita_hle_sceGxmShaderPatcherCreateVertexProgram(void) {
    gxm_program want;
    uint32_t attributes = r2, count = r3, streams = arg(4), stream_count = arg(5);
    memset(&want, 0, sizeof(want));
    want.header = r1;
    want.blend[0] = 15;
    for (uint32_t i = 0; i < count && i < 16 && vita_mem_ptr(attributes + 8 * i, 8); i++) {
        gxm_attribute *a = &want.attributes[want.attribute_count++];
        a->stream = vita_read16(attributes + 8 * i);
        a->offset = vita_read16(attributes + 8 * i + 2);
        a->format = vita_read8(attributes + 8 * i + 4);
        a->components = vita_read8(attributes + 8 * i + 5);
        a->reg = vita_read16(attributes + 8 * i + 6);
    }
    for (uint32_t i = 0; i < stream_count && i < 16 && vita_mem_ptr(streams + 4 * i, 4); i++) {
        want.streams[want.stream_count].stride = vita_read16(streams + 4 * i);
        want.streams[want.stream_count].index_source = vita_read16(streams + 4 * i + 2);
        want.stream_count++;
    }
    r0 = program_create(PROGRAM_VERTEX, &want, arg(6));
}

/* int sceGxmShaderPatcherCreateFragmentProgram(patcher, programId, SceGxmOutputRegisterFormat outputFormat,
 *         SceGxmMultisampleMode multisampleMode, const SceGxmBlendInfo *blendInfo,
 *         const SceGxmProgram *vertexProgram, SceGxmFragmentProgram **fragmentProgram): three on the stack
 * Blending is part of the program: { colour mask; colour and alpha function; colour factors; alpha factors }. */
void vita_hle_sceGxmShaderPatcherCreateFragmentProgram(void) {
    gxm_program want;
    uint32_t blend = arg(4);
    memset(&want, 0, sizeof(want));
    want.header = r1;
    want.output_format = r2;
    want.blend[0] = 15;
    if (blend && vita_mem_ptr(blend, 4)) memcpy(want.blend, vita_mem_ptr(blend, 4), 4);
    r0 = program_create(PROGRAM_FRAGMENT, &want, arg(6));
}

/* int sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(patcher, SceGxmFragmentProgram **fragmentProgram) */
void vita_hle_sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(void) {
    gxm_program want;
    memset(&want, 0, sizeof(want));
    r0 = program_create(PROGRAM_MASK, &want, r1);
}

static void program_ref(uint32_t obj, int by) { if (program_of(obj)) wr(obj + 12, rd(obj + 12) + (uint32_t)by); }
void vita_hle_sceGxmShaderPatcherAddRefVertexProgram(void)    { program_ref(r1, 1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherAddRefFragmentProgram(void)  { program_ref(r1, 1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherReleaseVertexProgram(void)   { program_ref(r1, -1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherReleaseFragmentProgram(void) { program_ref(r1, -1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetVertexProgramRefCount(void)   { wr(r2, program_of(r1) ? rd(r1 + 12) : 0); r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetFragmentProgramRefCount(void) { wr(r2, program_of(r1) ? rd(r1 + 12) : 0); r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetHostMemAllocated(void)   { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetBufferMemAllocated(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetVertexUsseMemAllocated(void)   { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetFragmentUsseMemAllocated(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherSetUserData(void) { wr(r0 + 4, r1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetUserData(void) { r0 = rd(r0 + 4); }

/* const SceGxmProgram *sceGxmVertexProgramGetProgram(const SceGxmVertexProgram *) */
void vita_hle_sceGxmVertexProgramGetProgram(void)   { r0 = rd(r0); }
void vita_hle_sceGxmFragmentProgramGetProgram(void) { r0 = rd(r0); }
void vita_hle_sceGxmFragmentProgramGetPassType(void) { r0 = 0; }
void vita_hle_sceGxmFragmentProgramIsEnabled(void)   { r0 = 1; }

/* --- precomputed draws and states --------------------------------------------------------
 * Each is a small structure of the title's and a block of its memory. A
 * draw is { vertex program; primitive; the block, an array of stream
 * pointers; how many; index format; indices; count; instances }. A state is
 * { program; the block; texture count | buffer count << 16; where in the
 * block the buffer pointers begin }: the block holds a 16-byte texture per
 * unit, then a pointer per uniform buffer, the default one first. */

enum { PD_PROGRAM = 0, PD_PRIMITIVE = 4, PD_STREAMS = 8, PD_STREAM_COUNT = 12, PD_INDEX_FORMAT = 16, PD_INDICES = 20,
       PD_COUNT = 24, PD_INSTANCES = 28, PD_BYTES = 44 };
enum { PS_PROGRAM = 0, PS_BLOCK = 4, PS_COUNTS = 8, PS_BUFFERS = 12 };

static uint32_t stream_count_of(const gxm_program *p) {
    uint32_t n = 0;
    for (uint32_t i = 0; p && i < p->attribute_count; i++) if (p->attributes[i].stream + 1u > n) n = p->attributes[i].stream + 1u;
    return n > 16 ? 16 : n;
}
static uint32_t texture_count_of(const gxm_program *p) {
    uint32_t n = 0;
    for (uint32_t u = 0; p && u < GXP_TEXTURE_UNITS; u++) if ((p->gxp.textures_used >> u) & 1) n = u + 1;
    return n;
}
/* none; or the default buffer and every user buffer up to the highest used */
static uint32_t buffer_count_of(const gxm_program *p) {
    uint32_t n = 0;
    if (!p || !p->gxp.buffers_used) return 0;
    for (uint32_t b = 0; b < GXP_DEFAULT_BUFFER; b++) if ((p->gxp.buffers_used >> b) & 1) n = b + 1;
    return n + 1;
}
static uint32_t state_size(const gxm_program *p) {
    uint32_t n = texture_count_of(p) * 16 + buffer_count_of(p) * 4;
    return n ? (n + 15) & ~15u : 16;
}

void vita_hle_sceGxmGetPrecomputedDrawSize(void) {
    uint32_t n = stream_count_of(program_of(r0)) * 4;
    r0 = n ? (n + 15) & ~15u : 16;
}
void vita_hle_sceGxmGetPrecomputedVertexStateSize(void)   { r0 = state_size(program_of(r0)); }
void vita_hle_sceGxmGetPrecomputedFragmentStateSize(void) { r0 = state_size(program_of(r0)); }

/* int sceGxmPrecomputedDrawInit(SceGxmPrecomputedDraw *draw, const SceGxmVertexProgram *vertexProgram, void *memBlock) */
void vita_hle_sceGxmPrecomputedDrawInit(void) {
    uint32_t d = r0, block = r2, n = stream_count_of(program_of(r1));
    if (!vita_mem_ptr(d, PD_BYTES) || !r1 || !block) { r0 = E_INVALID_POINTER; return; }
    if (block & 15) { r0 = E_INVALID_ALIGN; return; }
    memset(vita_mem_ptr(d, PD_BYTES), 0, PD_BYTES);
    vita_write32(d + PD_PROGRAM, r1);
    vita_write32(d + PD_STREAMS, block);
    vita_write32(d + PD_STREAM_COUNT, n);
    for (uint32_t i = 0; i < (n ? n : 1); i++) wr(block + 4 * i, 0);
    r0 = 0;
}
/* void sceGxmPrecomputedDrawSetParams(draw, primType, indexType, indexData, indexCount) */
void vita_hle_sceGxmPrecomputedDrawSetParams(void) {
    wr(r0 + PD_PRIMITIVE, r1); wr(r0 + PD_INDEX_FORMAT, r2); wr(r0 + PD_INDICES, r3);
    wr(r0 + PD_COUNT, arg(4)); wr(r0 + PD_INSTANCES, 1);
}
/* void sceGxmPrecomputedDrawSetParamsInstanced(draw, primType, indexType, indexData, indexCount, indexWrap) */
void vita_hle_sceGxmPrecomputedDrawSetParamsInstanced(void) {
    uint32_t count = arg(4), wrap = arg(5);
    wr(r0 + PD_PRIMITIVE, r1); wr(r0 + PD_INDEX_FORMAT, r2); wr(r0 + PD_INDICES, r3);
    wr(r0 + PD_COUNT, wrap); wr(r0 + PD_INSTANCES, wrap ? count / wrap : 0);
}
/* int sceGxmPrecomputedDrawSetVertexStream(draw, streamIndex, streamData) */
void vita_hle_sceGxmPrecomputedDrawSetVertexStream(void) {
    if (r1 > 15) { r0 = E_INVALID_VALUE; return; }
    if (r1 < rd(r0 + PD_STREAM_COUNT)) wr(rd(r0 + PD_STREAMS) + 4 * r1, r2);
    r0 = 0;
}
/* int sceGxmPrecomputedDrawSetAllVertexStreams(draw, const void * const *streamDataArray) */
void vita_hle_sceGxmPrecomputedDrawSetAllVertexStreams(void) {
    uint32_t n = rd(r0 + PD_STREAM_COUNT), block = rd(r0 + PD_STREAMS);
    for (uint32_t i = 0; i < n; i++) wr(block + 4 * i, rd(r1 + 4 * i));
    r0 = 0;
}

static void state_init(void) {
    uint32_t s = r0, block = r2;
    const gxm_program *p = program_of(r1);
    if (!vita_mem_ptr(s, 16) || !r1 || !block) { r0 = E_INVALID_POINTER; return; }
    if (block & 15) { r0 = E_INVALID_ALIGN; return; }
    uint32_t textures = texture_count_of(p), buffers = buffer_count_of(p), size = state_size(p);
    void *mem = vita_mem_ptr(block, size);
    if (mem) memset(mem, 0, size);
    vita_write32(s + PS_PROGRAM, r1);
    vita_write32(s + PS_BLOCK, block);
    vita_write32(s + PS_COUNTS, textures | (buffers << 16));
    vita_write32(s + PS_BUFFERS, block + 16 * textures);
    r0 = 0;
}
/* int sceGxmPrecomputedVertexStateInit(state, const SceGxmVertexProgram *vertexProgram, void *memBlock) */
void vita_hle_sceGxmPrecomputedVertexStateInit(void)   { state_init(); }
void vita_hle_sceGxmPrecomputedFragmentStateInit(void) { state_init(); }

static void state_set_texture(void) {
    if (r1 >= 16) { r0 = E_INVALID_VALUE; return; }
    const void *from = vita_mem_ptr(r2, 16);
    if (!from) { r0 = E_INVALID_POINTER; return; }
    void *to = r1 < (rd(r0 + PS_COUNTS) & 0xFFFF) ? vita_mem_ptr(rd(r0 + PS_BLOCK) + 16 * r1, 16) : NULL;
    if (to) memcpy(to, from, 16);
    r0 = 0;
}
static void state_set_all_textures(void) {
    uint32_t n = rd(r0 + PS_COUNTS) & 0xFFFF;
    void *to = n ? vita_mem_ptr(rd(r0 + PS_BLOCK), 16 * n) : NULL;
    const void *from = n ? vita_mem_ptr(r1, 16 * n) : NULL;
    if (to && from) memcpy(to, from, 16 * n);
    r0 = 0;
}
static void state_set_buffer(uint32_t s, uint32_t slot, uint32_t pointer) {
    if (slot < (rd(s + PS_COUNTS) >> 16)) wr(rd(s + PS_BUFFERS) + 4 * slot, pointer);
}
static void state_set_uniform_buffer(void) {
    if (r1 >= GXP_DEFAULT_BUFFER) { r0 = E_INVALID_VALUE; return; }
    state_set_buffer(r0, r1 + 1, r2);
    r0 = 0;
}
static void state_set_all_uniform_buffers(void) {
    uint32_t n = rd(r0 + PS_COUNTS) >> 16;
    for (uint32_t i = 1; i < n; i++) state_set_buffer(r0, i, rd(r1 + 4 * (i - 1)));
    r0 = 0;
}
/* int sceGxmPrecomputedFragmentStateSetTexture(state, textureIndex, const SceGxmTexture *texture): a copy */
void vita_hle_sceGxmPrecomputedFragmentStateSetTexture(void)          { state_set_texture(); }
void vita_hle_sceGxmPrecomputedVertexStateSetTexture(void)            { state_set_texture(); }
void vita_hle_sceGxmPrecomputedFragmentStateSetAllTextures(void)      { state_set_all_textures(); }
void vita_hle_sceGxmPrecomputedVertexStateSetAllTextures(void)        { state_set_all_textures(); }
/* int sceGxmPrecomputedFragmentStateSetUniformBuffer(state, bufferIndex, const void *bufferData) */
void vita_hle_sceGxmPrecomputedFragmentStateSetUniformBuffer(void)    { state_set_uniform_buffer(); }
void vita_hle_sceGxmPrecomputedVertexStateSetUniformBuffer(void)      { state_set_uniform_buffer(); }
void vita_hle_sceGxmPrecomputedFragmentStateSetAllUniformBuffers(void){ state_set_all_uniform_buffers(); }
void vita_hle_sceGxmPrecomputedVertexStateSetAllUniformBuffers(void)  { state_set_all_uniform_buffers(); }
/* int sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(state, void *defaultBuffer) */
void vita_hle_sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(void) { state_set_buffer(r0, 0, r1); r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(void)   { state_set_buffer(r0, 0, r1); r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer(void) { r0 = (rd(r0 + PS_COUNTS) >> 16) ? rd(rd(r0 + PS_BUFFERS)) : 0; }
void vita_hle_sceGxmPrecomputedVertexStateGetDefaultUniformBuffer(void)   { r0 = (rd(r0 + PS_COUNTS) >> 16) ? rd(rd(r0 + PS_BUFFERS)) : 0; }

/* void sceGxmSetPrecomputedVertexState(context, const SceGxmPrecomputedVertexState *state): NULL takes it away */
void vita_hle_sceGxmSetPrecomputedVertexState(void)   { g_ctx.precomputed[0] = r1; }
void vita_hle_sceGxmSetPrecomputedFragmentState(void) { g_ctx.precomputed[1] = r1; }

/* --- bindings ------------------------------------------------------------------------- */

/* void sceGxmSetVertexProgram(context, const SceGxmVertexProgram *vertexProgram) */
void vita_hle_sceGxmSetVertexProgram(void)   { if (r1) { g_ctx.program[0] = r1; g_state.vertex = program_of(r1); } }
void vita_hle_sceGxmSetFragmentProgram(void) { if (r1) { g_ctx.program[1] = r1; g_state.fragment = program_of(r1); } }

/* int sceGxmSetVertexStream(context, unsigned int streamIndex, const void *streamData) */
void vita_hle_sceGxmSetVertexStream(void) {
    if (r1 > 15) { r0 = E_INVALID_VALUE; return; }
    g_state.streams[r1] = r2;
    r0 = 0;
}

/* int sceGxmSetFragmentTexture(context, unsigned int textureIndex, const SceGxmTexture *texture): a copy */
static void set_texture(uint32_t table[GXP_TEXTURE_UNITS][4]) {
    if (r1 > 15) { r0 = E_INVALID_VALUE; return; }
    const void *from = vita_mem_ptr(r2, 16);
    if (!from) { r0 = E_INVALID_POINTER; return; }
    memcpy(table[r1], from, 16);
    r0 = 0;
}
void vita_hle_sceGxmSetVertexTexture(void)   { set_texture(g_state.vertex_textures); }
void vita_hle_sceGxmSetFragmentTexture(void) { set_texture(g_state.fragment_textures); }

/* int sceGxmSetVertexUniformBuffer(context, unsigned int bufferIndex, const void *bufferData) */
void vita_hle_sceGxmSetVertexUniformBuffer(void) {
    if (r1 >= GXP_DEFAULT_BUFFER) { r0 = E_INVALID_VALUE; return; }
    g_state.vertex_buffers[r1] = r2;
    r0 = 0;
}
void vita_hle_sceGxmSetFragmentUniformBuffer(void) {
    if (r1 >= GXP_DEFAULT_BUFFER) { r0 = E_INVALID_VALUE; return; }
    g_state.fragment_buffers[r1] = r2;
    r0 = 0;
}
void vita_hle_sceGxmSetVertexDefaultUniformBuffer(void)   { g_state.vertex_buffers[GXP_DEFAULT_BUFFER] = r1; r0 = 0; }
void vita_hle_sceGxmSetFragmentDefaultUniformBuffer(void) { g_state.fragment_buffers[GXP_DEFAULT_BUFFER] = r1; r0 = 0; }

/* int sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *context, void **uniformBuffer)
 * A default uniform buffer for the program now set, out of the ring the
 * title gave the context: as large as the program says, and the stage's
 * default buffer from here on. When the ring is spent it starts again:
 * what was drawn with the old contents is already drawn. */
static void reserve_default(int stage) {
    static uint32_t fallback[2];
    const gxm_program *p = stage ? g_state.fragment : g_state.vertex;
    uint32_t size = p ? p->gxp.default_buffer_words * 4 : 0, *slot = &(stage ? g_state.fragment_buffers : g_state.vertex_buffers)[GXP_DEFAULT_BUFFER];
    if (!vita_mem_ptr(r1, 4)) { r0 = E_INVALID_POINTER; return; }
    if (!size) { vita_write32(r1, 0); *slot = 0; r0 = 0; return; }
    if (!g_ctx.ring[stage] || g_ctx.ring_size[stage] < size) {
        /* a context made without a ring large enough: one of this runtime's */
        if (!fallback[stage]) fallback[stage] = vita_sysmem_reserve(0x100000);
        g_ctx.ring[stage] = fallback[stage];
        g_ctx.ring_size[stage] = fallback[stage] ? 0x100000 : 0;
        g_ctx.ring_at[stage] = 0;
        if (g_ctx.ring_size[stage] < size) { r0 = 0x805B000Bu; return; }
    }
    uint32_t at = (g_ctx.ring_at[stage] + 3) & ~3u;
    if (at + size > g_ctx.ring_size[stage]) at = 0;
    g_ctx.ring_at[stage] = at + size;
    *slot = g_ctx.ring[stage] + at;
    vita_write32(r1, *slot);
    r0 = 0;
}
void vita_hle_sceGxmReserveVertexDefaultUniformBuffer(void)   { reserve_default(0); }
void vita_hle_sceGxmReserveFragmentDefaultUniformBuffer(void) { reserve_default(1); }

/* --- raster state ----------------------------------------------------------------------
 * Enumerations arrive as the bit fields of a hardware word: a depth
 * function is its number shifted left by 22, a stencil function by 25, a
 * polygon mode by 15, and the switches are single bits. */

/* void sceGxmSetViewport(context, float xOffset, float xScale, float yOffset, float yScale, float zOffset, float zScale) */
void vita_hle_sceGxmSetViewport(void) {
    g_state.offset[0] = vfp_getf(0); g_state.scale[0] = vfp_getf(1);
    g_state.offset[1] = vfp_getf(2); g_state.scale[1] = vfp_getf(3);
    g_state.offset[2] = vfp_getf(4); g_state.scale[2] = vfp_getf(5);
}
void vita_hle_sceGxmSetViewportEnable(void) { g_state.viewport_on = !(r1 & 0x00010000u); }

/* The viewport of a clip space with y up and depth from -1 to 1, over
 * (0,0)..(xMax,yMax), and drawing confined to that. */
static void default_region_and_viewport(uint32_t x_max, uint32_t y_max) {
    g_state.region_mode = GXM_REGION_OUTSIDE;
    g_state.region[0] = g_state.region[1] = 0;
    g_state.region[2] = x_max; g_state.region[3] = y_max;
    g_state.offset[0] = g_state.scale[0] = (float)(x_max + 1) * 0.5f;
    g_state.offset[1] = (float)(y_max + 1) * 0.5f;
    g_state.scale[1] = -g_state.offset[1];
    g_state.offset[2] = g_state.scale[2] = 0.5f;
}
/* void sceGxmSetDefaultRegionClipAndViewport(context, unsigned int xMax, unsigned int yMax) */
void vita_hle_sceGxmSetDefaultRegionClipAndViewport(void) { default_region_and_viewport(r1, r2); }

/* void sceGxmSetRegionClip(context, SceGxmRegionClipMode mode, xMin, yMin, xMax, yMax) */
void vita_hle_sceGxmSetRegionClip(void) {
    g_state.region_mode = (int)(r1 >> 30);
    g_state.region[0] = r2; g_state.region[1] = r3;
    g_state.region[2] = arg(4); g_state.region[3] = arg(5);
}
void vita_hle_sceGxmSetCullMode(void) { g_state.cull = (int)(r1 & 3); }
void vita_hle_sceGxmSetTwoSidedEnable(void) { g_state.two_sided = r1 != 0; }
void vita_hle_sceGxmSetWClampEnable(void) { }
void vita_hle_sceGxmSetWClampValue(void) { }
void vita_hle_sceGxmSetWBufferEnable(void) { }

void vita_hle_sceGxmSetFrontDepthFunc(void) { g_state.face[0].depth_func = (r1 >> 22) & 7; }
void vita_hle_sceGxmSetBackDepthFunc(void)  { g_state.face[1].depth_func = (r1 >> 22) & 7; }
void vita_hle_sceGxmSetFrontDepthWriteEnable(void) { g_state.face[0].depth_write = !(r1 & 0x00100000u); }
void vita_hle_sceGxmSetBackDepthWriteEnable(void)  { g_state.face[1].depth_write = !(r1 & 0x00100000u); }
void vita_hle_sceGxmSetFrontDepthBias(void) { }
void vita_hle_sceGxmSetBackDepthBias(void) { }

/* void sceGxmSetFrontStencilFunc(context, func, stencilFail, depthFail, depthPass,
 *                                unsigned char compareMask, unsigned char writeMask): three on the stack */
static void stencil_func(gxm_face *f) {
    f->stencil_func = (r1 >> 25) & 7;
    f->stencil_fail = r2 & 7;
    f->depth_fail = r3 & 7;
    f->depth_pass = arg(4) & 7;
    f->compare_mask = (uint8_t)arg(5);
    f->write_mask = (uint8_t)arg(6);
}
void vita_hle_sceGxmSetFrontStencilFunc(void) { stencil_func(&g_state.face[0]); }
void vita_hle_sceGxmSetBackStencilFunc(void)  { stencil_func(&g_state.face[1]); }
void vita_hle_sceGxmSetFrontStencilRef(void) { g_state.face[0].stencil_ref = (uint8_t)r1; }
void vita_hle_sceGxmSetBackStencilRef(void)  { g_state.face[1].stencil_ref = (uint8_t)r1; }
void vita_hle_sceGxmSetFrontPolygonMode(void) { }
void vita_hle_sceGxmSetBackPolygonMode(void) { }
void vita_hle_sceGxmSetFrontPointLineWidth(void) { }
void vita_hle_sceGxmSetBackPointLineWidth(void) { }
void vita_hle_sceGxmSetFrontLineFillLastPixelEnable(void) { }
void vita_hle_sceGxmSetBackLineFillLastPixelEnable(void) { }
void vita_hle_sceGxmSetFrontFragmentProgramEnable(void) { g_state.face[0].program_off = (r1 & 0x00200000u) != 0; }
void vita_hle_sceGxmSetBackFragmentProgramEnable(void)  { g_state.face[1].program_off = (r1 & 0x00200000u) != 0; }
void vita_hle_sceGxmSetYuvProfile(void) { r0 = 0; }

/* The visibility buffer is counters, one slice of them per GPU core: the
 * pixels that passed the tests while a test was enabled, or just whether
 * any did. Everything is counted in the first slice. */
void vita_hle_sceGxmSetVisibilityBuffer(void) {
    if (r1 & 15) { r0 = E_INVALID_ALIGN; return; }
    g_ctx.visibility = r1;
    g_ctx.visibility_stride = r2;
    void *p = r1 && r2 ? vita_mem_ptr(r1, r2 * 4) : NULL;
    if (p) memset(p, 0, r2 * 4);
    r0 = 0;
}
void vita_hle_sceGxmSetFrontVisibilityTestEnable(void) { g_ctx.visibility_on[0] = r1 != 0; }
void vita_hle_sceGxmSetBackVisibilityTestEnable(void)  { g_ctx.visibility_on[1] = r1 != 0; }
void vita_hle_sceGxmSetFrontVisibilityTestIndex(void) { g_ctx.visibility_index[0] = r1; }
void vita_hle_sceGxmSetBackVisibilityTestIndex(void)  { g_ctx.visibility_index[1] = r1; }
void vita_hle_sceGxmSetFrontVisibilityTestOp(void) { g_ctx.visibility_set[0] = r1 != 0; }
void vita_hle_sceGxmSetBackVisibilityTestOp(void)  { g_ctx.visibility_set[1] = r1 != 0; }

/* --- scenes ----------------------------------------------------------------------------- */

/* What a frame asked for and what became of it, reported at each flip whose
 * shape differs from the flip before, and at every hundredth: a thousand
 * identical frames are ten lines, and a frame the renderer could not draw
 * all of says so. */
static struct { uint32_t scenes, draws, precomputed; gxm_draw_counters c; uint64_t draw_us; } g_frame, g_last;
static uint32_t g_frames;

static void frame_end(void) {
    g_frames++;
    gxm_draw_frame();
    g_frame.c = gxm_counters;
    memset(&gxm_counters, 0, sizeof(gxm_counters));
    if (g_frames % 100 == 0 ||
        g_frame.scenes != g_last.scenes || g_frame.draws != g_last.draws || g_frame.precomputed != g_last.precomputed ||
        g_frame.c.not_triangles != g_last.c.not_triangles || g_frame.c.no_program != g_last.c.no_program ||
        !g_frame.c.stopped != !g_last.c.stopped || !g_frame.c.no_texture != !g_last.c.no_texture) {
        vita_hle_trace("gxm frame %u: %u scenes, %u draws (%u precomputed), %u triangles, %u pixels in %u ms; not drawn: %u lines or points, "
                       "%u without a program; %u program runs stopped (last at instruction %u), %u samples of nothing",
                       g_frames, g_frame.scenes, g_frame.draws, g_frame.precomputed, g_frame.c.triangles, g_frame.c.pixels,
                       (unsigned)(g_frame.draw_us / 1000),
                       g_frame.c.not_triangles, g_frame.c.no_program, g_frame.c.stopped, g_frame.c.stopped_at, g_frame.c.no_texture);
        g_last = g_frame;
    }
    memset(&g_frame, 0, sizeof(g_frame));
}

/* VITARECOMP_GXM_FRAME=<n>: every draw of the nth frame on stderr: which
 * programs, how much geometry, how many pixels came of it, and the state
 * that most often explains a wrong picture. The programs are named as
 * VITARECOMP_GXP_DUMP names their files. */
static int draw_logged(void) {
    static int asked;
    static uint32_t frame;
    if (!asked) { const char *e = getenv("VITARECOMP_GXM_FRAME"); asked = 1; frame = e ? (uint32_t)strtoul(e, NULL, 0) : 0; }
    return frame && g_frames + 1 == frame;
}

/* VITARECOMP_GXM_SKIP=<first>-<last>: the draws of those frames are counted
 * and not drawn. A title that shows one costly scene for a fixed time, a
 * warning say, spends that time here on every run of a bring-up; this is
 * for getting past it. What the frames left out would have put in any
 * surface is not there afterwards, so the last of them should be some way
 * before the screen that matters. */
static int draw_skipped(void) {
    static int asked;
    static uint32_t first, last;
    if (!asked) {
        const char *e = getenv("VITARECOMP_GXM_SKIP");
        asked = 1;
        if (e && sscanf(e, "%u-%u", &first, &last) != 2) first = last = 0;
        if (first) vita_hle_trace("gxm: frames %u to %u are not drawn (VITARECOMP_GXM_SKIP)", first, last);
    }
    return first && g_frames + 1 >= first && g_frames + 1 <= last;
}

/* VITARECOMP_GXM_SCENES=<directory>, with VITARECOMP_GXM_FRAME: what each scene
 * of that frame left in its colour surface, as an image, numbered in the
 * order the scenes ended. A frame is a dozen passes into as many surfaces,
 * and the one on the display is only the last; this shows the others. Values
 * are shown from 0 to 1 whatever the format holds; a surface of two whole
 * words by the bytes of the first. */
static void dump_scene(void) {
    static const char *dir;
    static int asked;
    if (!asked) { asked = 1; dir = getenv("VITARECOMP_GXM_SCENES"); }
    if (!dir || !*dir || !draw_logged() || !g_state.has_colour) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/frame-%u-scene-%02u.ppm", dir, g_frames + 1, g_frame.scenes);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", g_state.width, g_state.height);
    for (uint32_t y = 0; y < g_state.height; y++)
        for (uint32_t x = 0; x < g_state.width; x++) {
            uint32_t at = gxm_pixel_offset(g_state.colour_layout, x, y, g_state.width, g_state.height,
                                           g_state.colour_stride, g_state.colour.bytes);
            float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (at + g_state.colour.bytes <= g_state.colour_avail) pixfmt_decode(&g_state.colour, g_state.colour_data + at, c);
            for (int k = 0; k < 3; k++) {
                float v = c[k];
                if (g_state.colour.f[0].kind == PIX_F32 && g_state.colour.fields == 2) {
                    uint32_t bits;
                    memcpy(&bits, &c[0], 4);            /* a geometry buffer: its first word is a colour in bytes */
                    v = (float)((bits >> (8 * k)) & 0xFF) / 255.0f;
                }
                fputc((int)((!(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f + 0.5f), f);
            }
        }
    fclose(f);
    fprintf(stderr, "gxm scene %u.%u: %ux%u, colour format bytes %u, written to %s\n", g_frames + 1, g_frame.scenes,
            g_state.width, g_state.height, g_state.colour.bytes, path);
}

static int depth_has_depth(uint32_t format)   { return format && format != 0x00022000u; }
static int depth_has_stencil_bytes(uint32_t format) { return format == 0x00022000u || format == 0x00066000u || format == 0x000EE000u; }

/* Depth, stencil and mask between the scene's planes and the title's
 * surface, in the surface's format: floats, floats with the mask bit on
 * top, 24 bits under 8 of stencil, or 16 bits. */
static void depth_transfer(int store) {
    uint32_t control = g_scene.depth_surface[0], format = control & DS_FORMAT_BITS;
    uint32_t stride = (((control >> 3) & 0xFF) + 1) * 32, layout = (control & DS_TILED) == DS_TILED ? GXM_SURFACE_TILED : GXM_SURFACE_LINEAR;
    uint32_t depth_at = g_scene.depth_surface[1], stencil_at = g_scene.depth_surface[2];
    uint32_t width = g_state.width, height = g_state.height;
    uint32_t bytes = format == 0x02444000u ? 2 : 4;
    uint32_t depth_avail = depth_at ? vita_mem_avail(depth_at) : 0, stencil_avail = stencil_at ? vita_mem_avail(stencil_at) : 0;
    uint8_t *depth = depth_avail && depth_has_depth(format) ? vita_mem_ptr(depth_at, 1) : NULL;
    uint8_t *stencil = stencil_avail && depth_has_stencil_bytes(format) ? vita_mem_ptr(stencil_at, 1) : NULL;
    if (width > stride) width = stride;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            size_t i = (size_t)y * g_state.width + x;
            uint32_t at = gxm_pixel_offset(layout, x, y, stride, height, stride, 1);
            if (depth && at * bytes + bytes <= depth_avail) {
                uint8_t *p = depth + at * bytes;
                uint32_t w = 0;
                float f;
                if (store) {
                    f = g_depth[i];
                    if (format == 0x02444000u)      w = f >= 1.0f ? 0xFFFFu : f > 0.0f ? (uint32_t)(f * 65535.0f + 0.5f) : 0u;
                    else if (format == 0x01266000u) {
                        double scaled = (double)f * 16777215.0 + 0.5;
                        w = (scaled >= 16777215.0 ? 0xFFFFFFu : scaled > 0.0 ? (uint32_t)scaled : 0u) | ((uint32_t)g_stencil[i] << 24);
                    }
                    else {
                        memcpy(&w, &f, 4);
                        if (format == 0x000CC000u || format == 0x000EE000u) w = (w & 0x7FFFFFFFu) | ((uint32_t)g_mask[i] << 31);
                    }
                    memcpy(p, &w, bytes);
                } else {
                    memcpy(&w, p, bytes);
                    if (format == 0x02444000u)      g_depth[i] = (float)((double)w / 65535.0);
                    else if (format == 0x01266000u) { g_depth[i] = (float)((double)(w & 0xFFFFFFu) / 16777215.0); g_stencil[i] = (uint8_t)(w >> 24); }
                    else {
                        uint32_t v = w;
                        if (format == 0x000CC000u || format == 0x000EE000u) { g_mask[i] = (uint8_t)(w >> 31); v &= 0x7FFFFFFFu; }
                        memcpy(&f, &v, 4);
                        g_depth[i] = f;
                    }
                }
            }
            if (stencil && at < stencil_avail) {
                if (store) stencil[at] = g_stencil[i]; else g_stencil[i] = stencil[at];
            }
        }
}

/* int sceGxmBeginScene(context, flags, const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
 *                      SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject,
 *                      const SceGxmColorSurface *colorSurface, const SceGxmDepthStencilSurface *depthStencil)
 * The colour surface is drawn into where it lies, starting from what it
 * holds. Depth, stencil and mask start from the depth surface's background
 * values unless it asks to be loaded. The viewport and the region clip are
 * put back to the whole target; everything else carries over. */
void vita_hle_sceGxmBeginScene(void) {
    static uint32_t seen[64][6], nseen;
    uint32_t rt = r2, region = r3, cs = arg(6), ds = arg(7);
    if (!r0 || !vita_mem_ptr(rt, 20)) { r0 = E_INVALID_POINTER; return; }
    if (g_scene.active) { r0 = E_WITHIN_SCENE; return; }

    uint32_t width = vita_read16(rt + 4), height = vita_read16(rt + 6);
    g_state.has_colour = 0;
    if (cs && vita_mem_ptr(cs, CS_BYTES) && (rd(cs + CS_FLAGS) & 1) && pixfmt_colour(rd(cs + CS_FORMAT), &g_state.colour)) {
        uint32_t type = rd(cs + CS_TYPE), data = rd(cs + CS_DATA), sw = rd(cs + CS_SIZE) & 0xFFFF, sh = rd(cs + CS_SIZE) >> 16;
        g_state.colour_layout = type == 0x08000000u ? GXM_SURFACE_SWIZZLED : type == 0x04000000u ? GXM_SURFACE_TILED : GXM_SURFACE_LINEAR;
        g_state.colour_stride = rd(cs + CS_STRIDE);
        g_state.colour_gamma = (rd(cs + CS_FLAGS) >> 4) & 3;
        g_state.colour_avail = vita_mem_avail(data);
        g_state.colour_data = g_state.colour_avail ? vita_mem_ptr(data, 1) : NULL;
        g_state.has_colour = g_state.colour_data != NULL;
        if (sw < width) width = sw;
        if (sh < height) height = sh;
    }
    g_state.width = width;
    g_state.height = height;

    /* the planes beside the scene */
    size_t pixels = (size_t)width * height;
    if (pixels > g_plane_cap) {
        float *d = realloc(g_depth, pixels * sizeof(float));
        if (d) g_depth = d;
        uint8_t *s = realloc(g_stencil, pixels);
        if (s) g_stencil = s;
        uint8_t *m = realloc(g_mask, pixels);
        if (m) g_mask = m;
        if (!d || !s || !m) { r0 = E_OUT_OF_MEMORY; return; }
        g_plane_cap = pixels;
    }
    g_state.depth = g_depth; g_state.stencil = g_stencil; g_state.mask = g_mask;

    float background = 1.0f;
    uint32_t background2 = 0x100u;
    g_scene.has_depth_surface = 0;
    if (ds && vita_mem_ptr(ds, DS_BYTES)) {
        for (int i = 0; i < 5; i++) g_scene.depth_surface[i] = vita_read32(ds + 4u * (uint32_t)i);
        memcpy(&background, &g_scene.depth_surface[3], 4);
        background2 = g_scene.depth_surface[4];
        g_scene.has_depth_surface = (g_scene.depth_surface[0] & DS_FORMAT_BITS) != 0;
    }
    uint32_t depth_format = g_scene.has_depth_surface ? g_scene.depth_surface[0] & DS_FORMAT_BITS : 0;
    g_state.depth_bits = depth_format == 0x01266000u ? 24 : depth_format == 0x02444000u ? 16 : 0;
    for (size_t i = 0; i < pixels; i++) g_depth[i] = background;
    memset(g_stencil, (int)(background2 & 0xFF), pixels);
    memset(g_mask, (int)((background2 >> 8) & 1), pixels);
    if (g_scene.has_depth_surface && (g_scene.depth_surface[0] & 2)) depth_transfer(0);

    uint32_t x_max = width ? width - 1 : 0, y_max = height ? height - 1 : 0;
    if (region && vita_mem_ptr(region, 8)) { x_max = vita_read32(region); y_max = vita_read32(region + 4); }
    default_region_and_viewport(x_max, y_max);

    /* each kind of scene a title renders is described once */
    uint32_t key[6] = { width | height << 16, vita_read16(rt + 10), cs ? rd(cs + CS_FORMAT) : 0xFFFFFFFFu, cs ? rd(cs + CS_TYPE) : 0,
                        g_scene.has_depth_surface ? g_scene.depth_surface[0] : 0, cs ? rd(cs + CS_STRIDE) : 0 };
    uint32_t k;
    for (k = 0; k < nseen && memcmp(seen[k], key, sizeof(key)); k++) {}
    if (k == nseen && nseen < 64) {
        memcpy(seen[nseen++], key, sizeof(key));
        vita_hle_trace("gxm scene kind %u: target %ux%u multisample %u; colour %s format 0x%08X type 0x%X stride %u at 0x%08X; "
                       "depth %s format 0x%X stride %u%s%s at 0x%08X",
                       nseen, width, height, key[1], g_state.has_colour ? "surface" : "none", key[2], key[3], key[5], cs ? rd(cs + CS_DATA) : 0,
                       g_scene.has_depth_surface ? "surface" : "none", key[4] & DS_FORMAT_BITS, (((key[4] >> 3) & 0xFF) + 1) * 32,
                       (key[4] & 2) ? ", loaded" : "", (key[4] & 4) ? ", stored" : "", g_scene.has_depth_surface ? g_scene.depth_surface[1] : 0);
    }
    g_scene.active = 1;
    g_frame.scenes++;
    r0 = 0;
}

/* int sceGxmEndScene(context, const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification) */
void vita_hle_sceGxmEndScene(void) {
    if (!g_scene.active) { r0 = E_NOT_WITHIN_SCENE; return; }
    if (g_scene.has_depth_surface && (g_scene.depth_surface[0] & 4)) depth_transfer(1);
    dump_scene();
    g_scene.active = 0;
    notify(r1);
    notify(r2);
    r0 = 0;
}
/* int sceGxmMidSceneFlush(context, flags, vertexSyncObject, const SceGxmNotification *vertexNotification) */
void vita_hle_sceGxmMidSceneFlush(void) { notify(r3); r0 = 0; }
void vita_hle_sceGxmFinish(void) { }

/* --- draws ------------------------------------------------------------------------------ */

static void draw(gxm_draw_state *s, uint32_t primitive, uint32_t index_format, uint32_t indices, uint32_t count, uint32_t instances) {
    if (draw_skipped()) { g_frame.draws++; return; }
    uint32_t triangles = gxm_counters.triangles;
    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);
    uint32_t drawn = gxm_draw(s, primitive >> 26, index_format ? 4 : 2, indices, count, instances);
    timespec_get(&t1, TIME_UTC);
    uint64_t took = (uint64_t)((int64_t)(t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
    g_frame.draw_us += took;
    g_frame.draws++;
    if (draw_logged()) {
        const gxm_face *f = &s->face[0];
        fprintf(stderr, "gxm draw %u.%u: v_%016llx f_%016llx prim %u, %u indices x %u, %u triangles, %u pixels, %u us; "
                        "depth %u%s stencil %u ref %u ops %u/%u/%u masks %02X/%02X cull %d%s%s blend %02X%02X%02X%02X out %u; "
                        "viewport %s %.1f+%.1f %.1f+%.1f %.3f+%.3f region %d %u,%u-%u,%u\n",
                g_frame.scenes, g_frame.draws, (unsigned long long)(s->vertex ? s->vertex->name : 0),
                (unsigned long long)(s->fragment ? s->fragment->name : 0), primitive >> 26, count, instances,
                gxm_counters.triangles - triangles, drawn, (unsigned)took,
                f->depth_func, f->depth_write ? "w" : "", f->stencil_func, f->stencil_ref, f->stencil_fail, f->depth_fail, f->depth_pass,
                f->compare_mask, f->write_mask, s->cull, s->two_sided ? " two-sided" : "", f->program_off ? " no-program" : "",
                s->fragment ? s->fragment->blend[0] : 0, s->fragment ? s->fragment->blend[1] : 0,
                s->fragment ? s->fragment->blend[2] : 0, s->fragment ? s->fragment->blend[3] : 0,
                s->fragment ? s->fragment->output_format : 0, s->viewport_on ? "on" : "off",
                (double)s->offset[0], (double)s->scale[0], (double)s->offset[1], (double)s->scale[1],
                (double)s->offset[2], (double)s->scale[2], s->region_mode, s->region[0], s->region[1], s->region[2], s->region[3]);
    }
    /* the visibility test counts what the draw put on the screen */
    if (g_ctx.visibility && g_ctx.visibility_on[0] && g_ctx.visibility_index[0] * 4 + 4 <= g_ctx.visibility_stride) {
        uint32_t at = g_ctx.visibility + 4 * g_ctx.visibility_index[0];
        if (g_ctx.visibility_set[0]) { if (drawn) wr(at, 1); }
        else wr(at, rd(at) + drawn);
    }
}

/* int sceGxmDraw(context, SceGxmPrimitiveType primType, SceGxmIndexFormat indexType, const void *indexData,
 *                unsigned int indexCount): the count on the stack */
void vita_hle_sceGxmDraw(void) {
    if (!g_scene.active) { r0 = E_NOT_WITHIN_SCENE; return; }
    if (!r3) { r0 = E_INVALID_POINTER; return; }
    if (!g_state.vertex || !g_state.fragment) { r0 = E_NULL_PROGRAM; return; }
    draw(&g_state, r1, r2, r3, arg(4), 1);
    r0 = 0;
}

/* int sceGxmDrawInstanced(context, primType, indexType, indexData, indexCount, indexWrap): the buffer holds
 * indexWrap indices, drawn once for each of indexCount / indexWrap instances */
void vita_hle_sceGxmDrawInstanced(void) {
    uint32_t count = arg(4), wrap = arg(5);
    if (!g_scene.active) { r0 = E_NOT_WITHIN_SCENE; return; }
    if (!r3) { r0 = E_INVALID_POINTER; return; }
    if (!g_state.vertex || !g_state.fragment) { r0 = E_NULL_PROGRAM; return; }
    if (wrap) draw(&g_state, r1, r2, r3, wrap, count / wrap);
    r0 = 0;
}

/* A precomputed state's bindings over the context's: its program, its
 * textures and its uniform buffers, the default one in the first slot. */
static void apply_state(uint32_t state, const gxm_program **program, uint32_t textures[GXP_TEXTURE_UNITS][4],
                        uint32_t buffers[GXP_BUFFERS], int take_program) {
    uint32_t counts = rd(state + PS_COUNTS), ntex = counts & 0xFFFF, nbuf = counts >> 16;
    uint32_t block = rd(state + PS_BLOCK), pointers = rd(state + PS_BUFFERS);
    if (take_program && program_of(rd(state + PS_PROGRAM))) *program = program_of(rd(state + PS_PROGRAM));
    const void *t = ntex && ntex <= GXP_TEXTURE_UNITS ? vita_mem_ptr(block, 16 * ntex) : NULL;
    if (t) memcpy(textures, t, 16 * ntex);
    if (nbuf) buffers[GXP_DEFAULT_BUFFER] = rd(pointers);
    for (uint32_t i = 1; i < nbuf && i <= GXP_DEFAULT_BUFFER; i++) buffers[i - 1] = rd(pointers + 4 * i);
}

/* int sceGxmDrawPrecomputed(context, const SceGxmPrecomputedDraw *draw)
 * The draw brings its vertex program, streams and indices; the precomputed
 * states in force, if any, bring textures and uniform buffers and the
 * fragment program; the rest is the context's, which the draw leaves as it
 * found it. */
void vita_hle_sceGxmDrawPrecomputed(void) {
    static gxm_draw_state s;
    uint32_t d = r1;
    if (!vita_mem_ptr(d, PD_BYTES)) { r0 = E_INVALID_POINTER; return; }
    if (!g_scene.active) { r0 = E_NOT_WITHIN_SCENE; return; }
    s = g_state;
    if (program_of(rd(d + PD_PROGRAM))) s.vertex = program_of(rd(d + PD_PROGRAM));
    uint32_t streams = rd(d + PD_STREAMS), n = rd(d + PD_STREAM_COUNT);
    for (uint32_t i = 0; i < n && i < 16; i++) s.streams[i] = rd(streams + 4 * i);
    if (g_ctx.precomputed[0]) apply_state(g_ctx.precomputed[0], &s.vertex, s.vertex_textures, s.vertex_buffers, 0);
    if (g_ctx.precomputed[1]) apply_state(g_ctx.precomputed[1], &s.fragment, s.fragment_textures, s.fragment_buffers, 1);
    if (!s.vertex || !s.fragment) { r0 = E_NULL_PROGRAM; return; }
    g_frame.precomputed++;
    draw(&s, rd(d + PD_PRIMITIVE), rd(d + PD_INDEX_FORMAT), rd(d + PD_INDICES), rd(d + PD_COUNT), rd(d + PD_INSTANCES));
    r0 = 0;
}

/* --- transfers: not drawn through the pipeline, and not yet done at all ----------------- */

void vita_hle_sceGxmTransferCopy(void)     { r0 = 0; }
void vita_hle_sceGxmTransferDownscale(void){ r0 = 0; }
void vita_hle_sceGxmTransferFill(void)     { r0 = 0; }
void vita_hle_sceGxmTransferFinish(void)   { r0 = 0; }
