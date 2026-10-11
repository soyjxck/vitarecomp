/* hle_gxm.c — SceGxm, the graphics library, as a GPU that draws nothing.
 *
 * The first thing a title needs from the GPU is not pixels but consent:
 * initialisation that succeeds, objects it can create and pass around,
 * shaders it can register and query, a display queue that calls its swap
 * callback so the frame counter advances and the game loop turns. This file
 * is that. Every draw is accepted and discarded; every state setter is kept
 * where a later renderer will want it. What it keeps faithfully:
 *
 *   - the texture control words, in the hardware's layout (Vita3K's
 *     gxm/types.h), because a title reads them back through the Get calls
 *     and a renderer will read them whole;
 *   - the GXP program header, read in place from the title's own data for
 *     parameter queries, since the title finds its uniforms by name;
 *   - the display queue callback, called with a copy of the title's data as
 *     the driver would, so sceDisplaySetFrameBuf is reached from the
 *     title's own code.
 *
 * Objects the title only passes back (contexts, patchers, render targets,
 * sync objects, programs) are small records in the memory-block arena.
 * Calling convention: see hle.h; arguments past r3 are on the stack.
 */

#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_sysmem.h"

#include <string.h>

#define SCE_GXM_ERROR_INVALID_VALUE   0x805B0003u
#define SCE_GXM_ERROR_INVALID_POINTER 0x805B0004u
#define SCE_GXM_ERROR_OUT_OF_MEMORY   0x805B0002u

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

static void frame_end(void);

/* --- initialisation and memory ------------------------------------------------- */

static uint32_t g_display_cb, g_display_cb_size, g_display_cb_buf;
static uint32_t g_notification_region;

/* int sceGxmInitialize(const SceGxmInitializeParams *params)
 * { flags; displayQueueMaxPendingCount; displayQueueCallback;
 *   displayQueueCallbackDataSize; parameterBufferSize } */
void vita_hle_sceGxmInitialize(void) {
    if (!vita_mem_ptr(r0, 20)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    g_display_cb      = vita_read32(r0 + 8);
    g_display_cb_size = vita_read32(r0 + 12);
    g_display_cb_buf  = object(g_display_cb_size ? g_display_cb_size : 16);
    vita_hle_trace("sceGxmInitialize(pending %u, callback 0x%08X, data %u bytes, parameter buffer 0x%X)",
                   vita_read32(r0 + 4), g_display_cb, g_display_cb_size, vita_read32(r0 + 16));
    r0 = 0;
}
void vita_hle_sceGxmTerminate(void) { r0 = 0; }

/* int sceGxmMapMemory(void *base, SceSize size, SceGxmMemoryAttribFlags attr): the GPU can see all of it */
void vita_hle_sceGxmMapMemory(void)   { vita_hle_trace("sceGxmMapMemory(0x%08X, 0x%X, %u)", r0, r1, r2); r0 = 0; }
void vita_hle_sceGxmUnmapMemory(void) { r0 = 0; }

/* int sceGxmMapVertexUsseMemory(void *base, SceSize size, unsigned int *offset): the USSE offset is 0 */
void vita_hle_sceGxmMapVertexUsseMemory(void)     { if (vita_mem_ptr(r2, 4)) vita_write32(r2, 0); r0 = 0; }
void vita_hle_sceGxmMapFragmentUsseMemory(void)   { if (vita_mem_ptr(r2, 4)) vita_write32(r2, 0); r0 = 0; }
void vita_hle_sceGxmUnmapVertexUsseMemory(void)   { r0 = 0; }
void vita_hle_sceGxmUnmapFragmentUsseMemory(void) { r0 = 0; }

/* volatile unsigned int *sceGxmGetNotificationRegion(void): 512 words the
 * title and the GPU signal each other through; nothing here ever writes
 * them except the title, and a wait on one returns at once. */
void vita_hle_sceGxmGetNotificationRegion(void) {
    if (!g_notification_region) g_notification_region = object(0x1000);
    r0 = g_notification_region;
}

/* int sceGxmNotificationWait(const SceGxmNotification *notification): { address; value } */
void vita_hle_sceGxmNotificationWait(void) {
    if (vita_mem_ptr(r0, 8)) {
        uint32_t addr = vita_read32(r0), value = vita_read32(r0 + 4);
        if (addr && vita_mem_ptr(addr, 4)) vita_write32(addr, value);   /* the GPU got there */
    }
    r0 = 0;
}

/* --- contexts, sync objects, render targets ------------------------------------- */

/* int sceGxmCreateContext(const SceGxmContextParams *params, SceGxmContext **context):
 * the context lives in the host memory the title provides, as it does on the console. */
void vita_hle_sceGxmCreateContext(void) {
    if (!vita_mem_ptr(r0, 44) || !vita_mem_ptr(r1, 4)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    uint32_t ctx = vita_read32(r0);
    if (!ctx) ctx = object(0x100);
    vita_write32(r1, ctx);
    vita_hle_trace("sceGxmCreateContext(host 0x%08X) -> 0x%08X", vita_read32(r0), ctx);
    r0 = 0;
}
void vita_hle_sceGxmDestroyContext(void) { r0 = 0; }

/* int sceGxmSyncObjectCreate(SceGxmSyncObject **syncObject) */
void vita_hle_sceGxmSyncObjectCreate(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    vita_write32(r0, object(16));
    r0 = 0;
}
void vita_hle_sceGxmSyncObjectDestroy(void) { r0 = 0; }

/* int sceGxmGetRenderTargetMemSize(const SceGxmRenderTargetParams *params, unsigned int *driverMemSize) */
void vita_hle_sceGxmGetRenderTargetMemSize(void) {
    if (vita_mem_ptr(r1, 4)) vita_write32(r1, 0x1000);
    r0 = 0;
}

/* int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget)
 * The record keeps the params (20 bytes) for the title's later questions. */
void vita_hle_sceGxmCreateRenderTarget(void) {
    if (!vita_mem_ptr(r0, 20) || !vita_mem_ptr(r1, 4)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    uint32_t rt = object(32);
    if (!rt) { r0 = SCE_GXM_ERROR_OUT_OF_MEMORY; return; }
    for (uint32_t k = 0; k < 20; k += 4) vita_write32(rt + k, vita_read32(r0 + k));
    vita_write32(r1, rt);
    vita_hle_trace("sceGxmCreateRenderTarget(%ux%u) -> 0x%08X", vita_read16(r0 + 4), vita_read16(r0 + 6), rt);
    r0 = 0;
}
void vita_hle_sceGxmDestroyRenderTarget(void) { r0 = 0; }

/* int sceGxmRenderTargetGetDriverMemBlock(const SceGxmRenderTarget *renderTarget, SceUID *driverMemBlock) */
void vita_hle_sceGxmRenderTargetGetDriverMemBlock(void) {
    if (vita_mem_ptr(r0, 20) && vita_mem_ptr(r1, 4)) vita_write32(r1, vita_read32(r0 + 16));
    r0 = 0;
}

/* --- surfaces ------------------------------------------------------------------------
 * SceGxmColorSurface is 32 bytes of the title's; the layout here is this
 * runtime's: { data, colorFormat, width, height, strideInPixels, surfaceType,
 * scaleMode, gamma | clip } — what the Get calls answer and a renderer reads. */

/* int sceGxmColorSurfaceInit(surface, colorFormat, surfaceType, scaleMode, outputRegisterSize,
 *                            width, height, strideInPixels, data): five arguments on the stack */
void vita_hle_sceGxmColorSurfaceInit(void) {
    uint32_t s = r0;
    if (!vita_mem_ptr(s, 32)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    uint32_t width = arg(5), height = arg(6), stride = arg(7), data = arg(8);
    vita_write32(s + 0,  data);
    vita_write32(s + 4,  r1);
    vita_write32(s + 8,  width);
    vita_write32(s + 12, height);
    vita_write32(s + 16, stride);
    vita_write32(s + 20, r2);
    vita_write32(s + 24, r3);
    vita_write32(s + 28, 0);
    vita_hle_trace("sceGxmColorSurfaceInit(0x%08X, format 0x%08X, %ux%u stride %u, data 0x%08X)", s, r1, width, height, stride, data);
    r0 = 0;
}
void vita_hle_sceGxmColorSurfaceInitDisabled(void) { if (vita_mem_ptr(r0, 32)) for (uint32_t k = 0; k < 32; k += 4) vita_write32(r0 + k, 0); r0 = 0; }
void vita_hle_sceGxmColorSurfaceGetData(void)       { r0 = vita_mem_ptr(r0, 32) ? vita_read32(r0) : 0; }
void vita_hle_sceGxmColorSurfaceSetData(void)       { if (vita_mem_ptr(r0, 32)) vita_write32(r0, r1); r0 = 0; }
void vita_hle_sceGxmColorSurfaceGetFormat(void)     { r0 = vita_mem_ptr(r0, 32) ? vita_read32(r0 + 4) : 0; }
void vita_hle_sceGxmColorSurfaceGetStrideInPixels(void) { r0 = vita_mem_ptr(r0, 32) ? vita_read32(r0 + 16) : 0; }
void vita_hle_sceGxmColorSurfaceSetClip(void)       { if (vita_mem_ptr(r0, 32)) vita_write32(r0 + 28, (vita_read32(r0 + 28) & 0xF) | (r1 << 4) | (r2 << 12) | (r3 << 20)); r0 = 0; }
void vita_hle_sceGxmColorSurfaceSetGammaMode(void)  { if (vita_mem_ptr(r0, 32)) vita_write32(r0 + 28, (vita_read32(r0 + 28) & ~0xFu) | (r1 & 0xF)); r0 = 0; }
void vita_hle_sceGxmColorSurfaceGetGammaMode(void)  { r0 = vita_mem_ptr(r0, 32) ? (vita_read32(r0 + 28) & 0xF) : 0; }
void vita_hle_sceGxmColorSurfaceSetScaleMode(void)  { if (vita_mem_ptr(r0, 32)) vita_write32(r0 + 24, r1); }
void vita_hle_sceGxmColorSurfaceGetScaleMode(void)  { r0 = vita_mem_ptr(r0, 32) ? vita_read32(r0 + 24) : 0; }
void vita_hle_sceGxmColorSurfaceGetType(void)       { r0 = vita_mem_ptr(r0, 32) ? vita_read32(r0 + 20) : 0; }
void vita_hle_sceGxmColorSurfaceSetDitherMode(void) { r0 = 0; }
void vita_hle_sceGxmColorSurfaceGetDitherMode(void) { r0 = 0; }

/* SceGxmDepthStencilSurface, 16 bytes: { depthData, stencilData, format | type << 16, stride | load << 24 | store << 28 } */

/* int sceGxmDepthStencilSurfaceInit(surface, depthStencilFormat, surfaceType, strideInSamples,
 *                                   depthData, stencilData): two on the stack */
void vita_hle_sceGxmDepthStencilSurfaceInit(void) {
    uint32_t s = r0;
    if (!vita_mem_ptr(s, 16)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    vita_write32(s + 0,  arg(4));
    vita_write32(s + 4,  arg(5));
    vita_write32(s + 8,  (r1 & 0xFFFF) | (r2 << 16));
    vita_write32(s + 12, r3 & 0xFFFFFF);
    r0 = 0;
}
void vita_hle_sceGxmDepthStencilSurfaceInitDisabled(void) { if (vita_mem_ptr(r0, 16)) for (uint32_t k = 0; k < 16; k += 4) vita_write32(r0 + k, 0); r0 = 0; }
void vita_hle_sceGxmDepthStencilSurfaceSetForceLoadMode(void)  { if (vita_mem_ptr(r0, 16)) vita_write32(r0 + 12, (vita_read32(r0 + 12) & ~0x0F000000u) | ((r1 & 0xF) << 24)); r0 = 0; }
void vita_hle_sceGxmDepthStencilSurfaceSetForceStoreMode(void) { if (vita_mem_ptr(r0, 16)) vita_write32(r0 + 12, (vita_read32(r0 + 12) & ~0xF0000000u) | ((r1 & 0xF) << 28)); r0 = 0; }
void vita_hle_sceGxmDepthStencilSurfaceGetForceLoadMode(void)  { r0 = vita_mem_ptr(r0, 16) ? (vita_read32(r0 + 12) >> 24) & 0xF : 0; }
void vita_hle_sceGxmDepthStencilSurfaceGetForceStoreMode(void) { r0 = vita_mem_ptr(r0, 16) ? (vita_read32(r0 + 12) >> 28) & 0xF : 0; }
void vita_hle_sceGxmDepthStencilSurfaceGetFormat(void)         { r0 = vita_mem_ptr(r0, 16) ? vita_read32(r0 + 8) & 0xFFFF : 0; }
void vita_hle_sceGxmDepthStencilSurfaceGetStrideInSamples(void){ r0 = vita_mem_ptr(r0, 16) ? vita_read32(r0 + 12) & 0xFFFFFF : 0; }
void vita_hle_sceGxmDepthStencilSurfaceSetBackgroundDepth(void)   { r0 = 0; }
void vita_hle_sceGxmDepthStencilSurfaceSetBackgroundStencil(void) { r0 = 0; }
void vita_hle_sceGxmDepthStencilSurfaceSetBackgroundMask(void)    { r0 = 0; }

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

/* --- scenes and draws: accepted ----------------------------------------------------- */

/* What a frame asked for. Nothing is drawn, but the shape of a frame is the
 * first thing to know about a title that has reached its loop: whether it
 * draws at all, how much, and when that changes. The census is reported at
 * each flip whose numbers differ from the flip before, so a thousand
 * identical frames are one line. */
static struct { uint32_t scenes, draws, indices, instanced, precomputed, programs; } g_frame, g_last;
static uint32_t g_frames;

static void frame_end(void) {
    g_frames++;
    if (memcmp(&g_frame, &g_last, sizeof(g_frame)) != 0) {
        vita_hle_trace("gxm frame %u: %u scenes, %u draws (%u instanced, %u precomputed), %u indices, %u program changes",
                       g_frames, g_frame.scenes, g_frame.draws, g_frame.instanced, g_frame.precomputed,
                       g_frame.indices, g_frame.programs);
        g_last = g_frame;
    }
    memset(&g_frame, 0, sizeof(g_frame));
}

void vita_hle_sceGxmBeginScene(void) { g_frame.scenes++; r0 = 0; }
void vita_hle_sceGxmEndScene(void) {
    /* The notifications the title asked for are its own words; the GPU
     * writes them when the scene is done, which is now. */
    for (int i = 1; i <= 2; i++) {
        uint32_t n = i == 1 ? r1 : r2;
        if (n && vita_mem_ptr(n, 8)) {
            uint32_t addr = vita_read32(n), value = vita_read32(n + 4);
            if (addr && vita_mem_ptr(addr, 4)) vita_write32(addr, value);
        }
    }
    r0 = 0;
}
void vita_hle_sceGxmMidSceneFlush(void) { r0 = 0; }
void vita_hle_sceGxmFinish(void) { }
/* int sceGxmDraw(context, primitive type, index format, const void *indices, unsigned int count) */
void vita_hle_sceGxmDraw(void) { g_frame.draws++; g_frame.indices += arg(4); r0 = 0; }
/* int sceGxmDrawInstanced(context, primitive type, index format, indices, count, index wrap) */
void vita_hle_sceGxmDrawInstanced(void) { g_frame.draws++; g_frame.instanced++; g_frame.indices += arg(4); r0 = 0; }
void vita_hle_sceGxmDrawPrecomputed(void) { g_frame.draws++; g_frame.precomputed++; r0 = 0; }
void vita_hle_sceGxmSetVisibilityBuffer(void) { r0 = 0; }

/* Render state: void setters, every one of them. */
void vita_hle_sceGxmSetVertexProgram(void) { g_frame.programs++; }
void vita_hle_sceGxmSetFragmentProgram(void) { g_frame.programs++; }
void vita_hle_sceGxmSetVertexStream(void) { r0 = 0; }
void vita_hle_sceGxmSetVertexTexture(void) { r0 = 0; }
void vita_hle_sceGxmSetFragmentTexture(void) { r0 = 0; }
void vita_hle_sceGxmSetVertexUniformBuffer(void) { r0 = 0; }
void vita_hle_sceGxmSetFragmentUniformBuffer(void) { r0 = 0; }
void vita_hle_sceGxmSetViewport(void) { }
void vita_hle_sceGxmSetViewportEnable(void) { }
void vita_hle_sceGxmSetRegionClip(void) { }
void vita_hle_sceGxmSetCullMode(void) { }
void vita_hle_sceGxmSetWClampEnable(void) { }
void vita_hle_sceGxmSetWClampValue(void) { }
void vita_hle_sceGxmSetTwoSidedEnable(void) { }
void vita_hle_sceGxmSetFrontDepthFunc(void) { }
void vita_hle_sceGxmSetBackDepthFunc(void) { }
void vita_hle_sceGxmSetFrontDepthWriteEnable(void) { }
void vita_hle_sceGxmSetBackDepthWriteEnable(void) { }
void vita_hle_sceGxmSetFrontDepthBias(void) { }
void vita_hle_sceGxmSetBackDepthBias(void) { }
void vita_hle_sceGxmSetFrontStencilFunc(void) { }
void vita_hle_sceGxmSetBackStencilFunc(void) { }
void vita_hle_sceGxmSetFrontStencilRef(void) { }
void vita_hle_sceGxmSetBackStencilRef(void) { }
void vita_hle_sceGxmSetFrontPolygonMode(void) { }
void vita_hle_sceGxmSetBackPolygonMode(void) { }
void vita_hle_sceGxmSetFrontPointLineWidth(void) { }
void vita_hle_sceGxmSetBackPointLineWidth(void) { }
void vita_hle_sceGxmSetFrontFragmentProgramEnable(void) { }
void vita_hle_sceGxmSetBackFragmentProgramEnable(void) { }
void vita_hle_sceGxmSetFrontVisibilityTestEnable(void) { }
void vita_hle_sceGxmSetBackVisibilityTestEnable(void) { }
void vita_hle_sceGxmSetFrontVisibilityTestIndex(void) { }
void vita_hle_sceGxmSetBackVisibilityTestIndex(void) { }
void vita_hle_sceGxmSetFrontVisibilityTestOp(void) { }
void vita_hle_sceGxmSetBackVisibilityTestOp(void) { }
void vita_hle_sceGxmSetYuvProfile(void) { r0 = 0; }
void vita_hle_sceGxmSetDefaultRegionClipAndViewport(void) { }

/* --- uniform buffers --------------------------------------------------------------------
 * The default uniform buffer of each stage is reserved per draw from a ring
 * the driver owns; here a ring of 4 KB slots in the arena. */

static uint32_t uniform_slot(void) {
    static uint32_t ring, next;
    if (!ring) ring = vita_sysmem_reserve(0x100000);
    if (!ring) return 0;
    uint32_t at = ring + next;
    next = (next + 0x1000) & 0xFFFFF;
    return at;
}

/* int sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *context, void **uniformBuffer) */
void vita_hle_sceGxmReserveVertexDefaultUniformBuffer(void)   { if (vita_mem_ptr(r1, 4)) vita_write32(r1, uniform_slot()); r0 = 0; }
void vita_hle_sceGxmReserveFragmentDefaultUniformBuffer(void) { if (vita_mem_ptr(r1, 4)) vita_write32(r1, uniform_slot()); r0 = 0; }

/* --- programs: the GXP header, read where the title keeps it --------------------------
 * Offsets inside a GXP are relative to the field that holds them. The
 * parameter table is 16 bytes a parameter: { int32 name_offset; u8
 * category:4 | type:4; u8 component_count:4 | container_index:4; u8
 * semantic; u8 semantic_index; u32 array_size; u32 resource_index }. */

#define GXP_PARAMETER_COUNT   0x24
#define GXP_PARAMETERS_OFFSET 0x28
#define GXP_DEFAULT_UB_COUNT  0x6C

static uint32_t gxp_parameters(uint32_t program) {
    if (!vita_mem_ptr(program, 0x80)) return 0;
    return program + GXP_PARAMETERS_OFFSET + vita_read32(program + GXP_PARAMETERS_OFFSET);
}

/* unsigned int sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram *program): bytes */
void vita_hle_sceGxmProgramGetDefaultUniformBufferSize(void) {
    r0 = vita_mem_ptr(r0, 0x80) ? vita_read32(r0 + GXP_DEFAULT_UB_COUNT) * 4u : 0;
}
void vita_hle_sceGxmProgramGetParameterCount(void) { r0 = vita_mem_ptr(r0, 0x80) ? vita_read32(r0 + GXP_PARAMETER_COUNT) : 0; }

/* const SceGxmProgramParameter *sceGxmProgramGetParameter(const SceGxmProgram *program, unsigned int index) */
void vita_hle_sceGxmProgramGetParameter(void) {
    uint32_t table = gxp_parameters(r0);
    r0 = (table && r1 < vita_read32(r0 + GXP_PARAMETER_COUNT)) ? table + r1 * 16 : 0;
}

/* const SceGxmProgramParameter *sceGxmProgramFindParameterByName(const SceGxmProgram *program, const char *name) */
void vita_hle_sceGxmProgramFindParameterByName(void) {
    uint32_t table = gxp_parameters(r0), count = table ? vita_read32(r0 + GXP_PARAMETER_COUNT) : 0;
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

/* const char *sceGxmProgramParameterGetName(const SceGxmProgramParameter *parameter) */
void vita_hle_sceGxmProgramParameterGetName(void) { r0 = vita_mem_ptr(r0, 16) ? r0 + (uint32_t)(int32_t)vita_read32(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetCategory(void)       { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 4) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetType(void)           { r0 = vita_mem_ptr(r0, 16) ? (vita_read8(r0 + 4) >> 4) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetComponentCount(void) { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 5) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetContainerIndex(void) { r0 = vita_mem_ptr(r0, 16) ? (vita_read8(r0 + 5) >> 4) & 0xF : 0; }
void vita_hle_sceGxmProgramParameterGetSemantic(void)       { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 6) : 0; }
void vita_hle_sceGxmProgramParameterGetSemanticIndex(void)  { r0 = vita_mem_ptr(r0, 16) ? vita_read8(r0 + 7) : 0; }
void vita_hle_sceGxmProgramParameterGetArraySize(void)      { r0 = vita_mem_ptr(r0, 16) ? vita_read32(r0 + 8) : 0; }
void vita_hle_sceGxmProgramParameterGetResourceIndex(void)  { r0 = vita_mem_ptr(r0, 16) ? vita_read32(r0 + 12) : 0; }
void vita_hle_sceGxmProgramParameterIsSamplerCube(void)     { r0 = 0; }
void vita_hle_sceGxmProgramCheck(void)             { r0 = (vita_mem_ptr(r0, 4) && vita_read32(r0) == 0x00505847u) ? 0 : SCE_GXM_ERROR_INVALID_VALUE; }
void vita_hle_sceGxmProgramGetSize(void)           { r0 = vita_mem_ptr(r0, 0x80) ? vita_read32(r0 + 8) : 0; }
void vita_hle_sceGxmProgramGetType(void)           { r0 = vita_mem_ptr(r0, 0x80) ? (vita_read32(r0 + 0x10) & 1) : 0; }
void vita_hle_sceGxmProgramIsDiscardUsed(void)     { r0 = 0; }
void vita_hle_sceGxmProgramIsDepthReplaceUsed(void){ r0 = 0; }
void vita_hle_sceGxmProgramIsSpriteCoordUsed(void) { r0 = 0; }
void vita_hle_sceGxmProgramIsFragColorUsed(void)   { r0 = 1; }
void vita_hle_sceGxmProgramIsEquivalent(void)      { r0 = r0 == r1; }
void vita_hle_sceGxmProgramGetVertexProgramOutputs(void) { r0 = 0; }
void vita_hle_sceGxmProgramGetFragmentProgramInputs(void) { r0 = 0; }
void vita_hle_sceGxmProgramGetOutputRegisterFormat(void)  { r0 = 0; }

/* int sceGxmSetUniformDataF(void *uniformBuffer, const SceGxmProgramParameter *parameter,
 *                           unsigned int componentOffset, unsigned int componentCount, const float *sourceData):
 * into the buffer at the parameter's resource index, in floats. */
void vita_hle_sceGxmSetUniformDataF(void) {
    uint32_t buf = r0, param = r1, off = r2, count = r3, src = arg(4);
    if (!buf || !vita_mem_ptr(param, 16)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    uint32_t dst = buf + (vita_read32(param + 12) + off) * 4u;
    void *d = vita_mem_ptr(dst, count * 4u);
    const void *s = vita_mem_ptr(src, count * 4u);
    if (d && s) memcpy(d, s, count * 4u);
    r0 = 0;
}

/* --- the shader patcher ----------------------------------------------------------------
 * Ids are the program headers themselves; a vertex or fragment program is a
 * record { header } the title passes back. */

/* int sceGxmShaderPatcherCreate(const SceGxmShaderPatcherParams *params, SceGxmShaderPatcher **shaderPatcher) */
void vita_hle_sceGxmShaderPatcherCreate(void) {
    if (!vita_mem_ptr(r1, 4)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    vita_write32(r1, object(64));
    vita_hle_trace("sceGxmShaderPatcherCreate -> 0x%08X", vita_read32(r1));
    r0 = 0;
}
void vita_hle_sceGxmShaderPatcherDestroy(void) { r0 = 0; }

/* int sceGxmShaderPatcherRegisterProgram(patcher, const SceGxmProgram *programHeader, SceGxmShaderPatcherId *programId) */
void vita_hle_sceGxmShaderPatcherRegisterProgram(void) {
    if (!vita_mem_ptr(r2, 4)) { r0 = SCE_GXM_ERROR_INVALID_POINTER; return; }
    vita_write32(r2, r1);
    r0 = 0;
}
void vita_hle_sceGxmShaderPatcherUnregisterProgram(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherForceUnregisterProgram(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetProgramFromId(void) { /* the id is the header */ }

static uint32_t program_object(uint32_t header, uint32_t out) {
    if (!vita_mem_ptr(out, 4)) return SCE_GXM_ERROR_INVALID_POINTER;
    uint32_t o = object(32);
    if (!o) return SCE_GXM_ERROR_OUT_OF_MEMORY;
    vita_write32(o, header);
    vita_write32(out, o);
    return 0;
}

/* int sceGxmShaderPatcherCreateVertexProgram(patcher, programId, attributes, attributeCount,
 *                                            streams, streamCount, SceGxmVertexProgram **vertexProgram) */
void vita_hle_sceGxmShaderPatcherCreateVertexProgram(void)   { r0 = program_object(r1, arg(6)); }
/* int sceGxmShaderPatcherCreateFragmentProgram(patcher, programId, outputFormat, multisampleMode,
 *                                              blendInfo, vertexProgram, SceGxmFragmentProgram **fragmentProgram) */
void vita_hle_sceGxmShaderPatcherCreateFragmentProgram(void) { r0 = program_object(r1, arg(6)); }
void vita_hle_sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(void) { r0 = program_object(0, r1); }
void vita_hle_sceGxmShaderPatcherAddRefVertexProgram(void)    { r0 = 0; }
void vita_hle_sceGxmShaderPatcherAddRefFragmentProgram(void)  { r0 = 0; }
void vita_hle_sceGxmShaderPatcherReleaseVertexProgram(void)   { r0 = 0; }
void vita_hle_sceGxmShaderPatcherReleaseFragmentProgram(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetVertexProgramRefCount(void)   { if (vita_mem_ptr(r2, 4)) vita_write32(r2, 1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetFragmentProgramRefCount(void) { if (vita_mem_ptr(r2, 4)) vita_write32(r2, 1); r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetHostMemAllocated(void)   { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetBufferMemAllocated(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetVertexUsseMemAllocated(void)   { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetFragmentUsseMemAllocated(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherSetUserData(void) { r0 = 0; }
void vita_hle_sceGxmShaderPatcherGetUserData(void) { r0 = 0; }

/* const SceGxmProgram *sceGxmVertexProgramGetProgram(const SceGxmVertexProgram *) */
void vita_hle_sceGxmVertexProgramGetProgram(void)   { r0 = vita_mem_ptr(r0, 4) ? vita_read32(r0) : 0; }
void vita_hle_sceGxmFragmentProgramGetProgram(void) { r0 = vita_mem_ptr(r0, 4) ? vita_read32(r0) : 0; }
void vita_hle_sceGxmFragmentProgramGetPassType(void) { r0 = 0; }
void vita_hle_sceGxmFragmentProgramIsEnabled(void)   { r0 = 1; }

/* --- precomputed state: records the title fills, draws that are discarded ------------- */

void vita_hle_sceGxmGetPrecomputedDrawSize(void)          { r0 = 0x40; }
void vita_hle_sceGxmGetPrecomputedVertexStateSize(void)   { r0 = 0x40; }
void vita_hle_sceGxmGetPrecomputedFragmentStateSize(void) { r0 = 0x40; }
void vita_hle_sceGxmPrecomputedDrawInit(void)             { r0 = 0; }
void vita_hle_sceGxmPrecomputedDrawSetParams(void)        { r0 = 0; }
void vita_hle_sceGxmPrecomputedDrawSetParamsInstanced(void) { r0 = 0; }
void vita_hle_sceGxmPrecomputedDrawSetVertexStream(void)  { r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateInit(void)      { r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateInit(void)    { r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(void)   { r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(void) { r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateGetDefaultUniformBuffer(void)   { r0 = uniform_slot(); }
void vita_hle_sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer(void) { r0 = uniform_slot(); }
void vita_hle_sceGxmPrecomputedVertexStateSetUniformBuffer(void)      { r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateSetUniformBuffer(void)    { r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateSetAllUniformBuffers(void)  { r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateSetAllUniformBuffers(void){ r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateSetTexture(void)          { r0 = 0; }
void vita_hle_sceGxmPrecomputedFragmentStateSetAllTextures(void)      { r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateSetTexture(void)            { r0 = 0; }
void vita_hle_sceGxmPrecomputedVertexStateSetAllTextures(void)        { r0 = 0; }
void vita_hle_sceGxmSetPrecomputedVertexState(void)   { }
void vita_hle_sceGxmSetPrecomputedFragmentState(void) { }

/* --- textures: the hardware's control words --------------------------------------------
 * Word 0: unk:3 vaddr:3 uaddr:3 mip_filter:1 min:2 mag:2 unk:3 mip_count:4
 * lod_bias:6 gamma:2 unk:2 format0:1. Word 1: height-1:12 width-1:12 (or the
 * log2 sizes for swizzled) base_format:5 type:3. Word 2: lod_min:2 data>>2:30.
 * Word 3: palette>>6:26 lod_min:2 swizzle:3 normalize:1. A format value is
 * base_format << 24 | format0 << 31 | swizzle << 12. */

#define TEX_TYPE_SWIZZLED   0x00000000u
#define TEX_TYPE_CUBE       0x40000000u
#define TEX_TYPE_LINEAR     0x60000000u
#define TEX_TYPE_TILED      0x80000000u
#define TEX_TYPE_SWIZZLED_ARBITRARY 0xA0000000u
#define TEX_TYPE_CUBE_ARBITRARY     0xE0000000u

static uint32_t log2_u(uint32_t v) { uint32_t n = 0; while (v > 1) { v >>= 1; n++; } return n; }

static int texture_init(uint32_t t, uint32_t data, uint32_t format, uint32_t width, uint32_t height,
                        uint32_t mips, uint32_t type, uint32_t stride) {
    if (!vita_mem_ptr(t, 16)) return 0;
    uint32_t w0 = (1u << 10) | (1u << 12)            /* min and mag filters: linear; addressing: repeat */
                | (((mips ? mips - 1 : 0) & 0xF) << 16)
                | (format & 0x80000000u);
    uint32_t w1;
    if (type == TEX_TYPE_SWIZZLED || type == TEX_TYPE_CUBE)
        w1 = (log2_u(height) & 0xF) | ((log2_u(width) & 0xF) << 16);
    else
        w1 = ((height - 1) & 0xFFF) | (((width - 1) & 0xFFF) << 12);
    w1 |= ((format >> 24) & 0x1F) << 24;
    w1 |= type & 0xE0000000u;
    uint32_t w2 = data & ~3u;
    uint32_t w3 = ((format >> 12) & 7) << 28;
    (void)stride;                                  /* a strided texture's stride: with the renderer */
    vita_write32(t + 0, w0);
    vita_write32(t + 4, w1);
    vita_write32(t + 8, w2);
    vita_write32(t + 12, w3);
    return 1;
}

/* int sceGxmTextureInitLinear(texture, data, texFormat, width, height, mipCount): two on the stack */
void vita_hle_sceGxmTextureInitLinear(void)   { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), TEX_TYPE_LINEAR, 0) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }
void vita_hle_sceGxmTextureInitSwizzled(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), TEX_TYPE_SWIZZLED, 0) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }
void vita_hle_sceGxmTextureInitSwizzledArbitrary(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), TEX_TYPE_SWIZZLED_ARBITRARY, 0) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }
void vita_hle_sceGxmTextureInitTiled(void)    { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), TEX_TYPE_TILED, 0) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }
void vita_hle_sceGxmTextureInitCube(void)     { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), TEX_TYPE_CUBE, 0) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }
void vita_hle_sceGxmTextureInitCubeArbitrary(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), arg(5), TEX_TYPE_CUBE_ARBITRARY, 0) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }
/* int sceGxmTextureInitLinearStrided(texture, data, texFormat, width, height, byteStride) */
void vita_hle_sceGxmTextureInitLinearStrided(void) { r0 = texture_init(r0, r1, r2, r3, arg(4), 1, TEX_TYPE_LINEAR, arg(5)) ? 0 : SCE_GXM_ERROR_INVALID_POINTER; }

static uint32_t tw(uint32_t t, int i) { return vita_mem_ptr(t, 16) ? vita_read32(t + 4u * (uint32_t)i) : 0; }
static void tset(uint32_t t, int i, uint32_t mask, uint32_t value) {
    if (!vita_mem_ptr(t, 16)) return;
    vita_write32(t + 4u * (uint32_t)i, (vita_read32(t + 4u * (uint32_t)i) & ~mask) | (value & mask));
}

void vita_hle_sceGxmTextureGetType(void)   { r0 = tw(r0, 1) & 0xE0000000u; }
void vita_hle_sceGxmTextureGetFormat(void) { uint32_t w0 = tw(r0, 0), w1 = tw(r0, 1), w3 = tw(r0, 3); r0 = ((w1 >> 24) & 0x1F) << 24 | (w0 & 0x80000000u) | ((w3 >> 28) & 7) << 12; }
void vita_hle_sceGxmTextureGetWidth(void) {
    uint32_t w1 = tw(r0, 1), type = w1 & 0xE0000000u;
    r0 = (type == TEX_TYPE_SWIZZLED || type == TEX_TYPE_CUBE) ? 1u << ((w1 >> 16) & 0xF) : ((w1 >> 12) & 0xFFF) + 1;
}
void vita_hle_sceGxmTextureGetHeight(void) {
    uint32_t w1 = tw(r0, 1), type = w1 & 0xE0000000u;
    r0 = (type == TEX_TYPE_SWIZZLED || type == TEX_TYPE_CUBE) ? 1u << (w1 & 0xF) : (w1 & 0xFFF) + 1;
}
void vita_hle_sceGxmTextureGetData(void)    { r0 = tw(r0, 2) & ~3u; }
void vita_hle_sceGxmTextureSetData(void)    { tset(r0, 2, ~3u, r1); r0 = 0; }
void vita_hle_sceGxmTextureGetPalette(void) { r0 = tw(r0, 3) << 6; }
void vita_hle_sceGxmTextureSetPalette(void) { tset(r0, 3, 0x03FFFFFFu, r1 >> 6); r0 = 0; }
void vita_hle_sceGxmTextureGetMipmapCount(void)       { r0 = ((tw(r0, 0) >> 16) & 0xF) + 1; }
void vita_hle_sceGxmTextureGetMipmapCountUnsafe(void) { r0 = ((tw(r0, 0) >> 16) & 0xF) + 1; }
void vita_hle_sceGxmTextureSetMipmapCount(void)       { tset(r0, 0, 0xF << 16, ((r1 ? r1 - 1 : 0) & 0xF) << 16); r0 = 0; }
void vita_hle_sceGxmTextureGetMinFilter(void) { r0 = (tw(r0, 0) >> 10) & 3; }
void vita_hle_sceGxmTextureSetMinFilter(void) { tset(r0, 0, 3u << 10, (r1 & 3) << 10); r0 = 0; }
void vita_hle_sceGxmTextureGetMagFilter(void) { r0 = (tw(r0, 0) >> 12) & 3; }
void vita_hle_sceGxmTextureSetMagFilter(void) { tset(r0, 0, 3u << 12, (r1 & 3) << 12); r0 = 0; }
void vita_hle_sceGxmTextureGetMipFilter(void) { r0 = (tw(r0, 0) >> 9) & 1; }
void vita_hle_sceGxmTextureSetMipFilter(void) { tset(r0, 0, 1u << 9, (r1 & 1) << 9); r0 = 0; }
void vita_hle_sceGxmTextureGetUAddrMode(void) { r0 = (tw(r0, 0) >> 6) & 7; }
void vita_hle_sceGxmTextureSetUAddrMode(void) { tset(r0, 0, 7u << 6, (r1 & 7) << 6); r0 = 0; }
void vita_hle_sceGxmTextureGetVAddrMode(void) { r0 = (tw(r0, 0) >> 3) & 7; }
void vita_hle_sceGxmTextureSetVAddrMode(void) { tset(r0, 0, 7u << 3, (r1 & 7) << 3); r0 = 0; }
void vita_hle_sceGxmTextureGetUAddrModeSafe(void) { vita_hle_sceGxmTextureGetUAddrMode(); }
void vita_hle_sceGxmTextureSetUAddrModeSafe(void) { vita_hle_sceGxmTextureSetUAddrMode(); }
void vita_hle_sceGxmTextureGetVAddrModeSafe(void) { vita_hle_sceGxmTextureGetVAddrMode(); }
void vita_hle_sceGxmTextureSetVAddrModeSafe(void) { vita_hle_sceGxmTextureSetVAddrMode(); }
void vita_hle_sceGxmTextureGetLodBias(void)   { r0 = (tw(r0, 0) >> 20) & 0x3F; }
void vita_hle_sceGxmTextureSetLodBias(void)   { tset(r0, 0, 0x3Fu << 20, (r1 & 0x3F) << 20); r0 = 0; }
void vita_hle_sceGxmTextureGetLodMin(void)    { r0 = tw(r0, 2) & 3; }
void vita_hle_sceGxmTextureSetLodMin(void)    { tset(r0, 2, 3u, r1 & 3); r0 = 0; }
void vita_hle_sceGxmTextureGetGammaMode(void) { r0 = ((tw(r0, 0) >> 26) & 3) << 27; }
void vita_hle_sceGxmTextureSetGammaMode(void) { tset(r0, 0, 3u << 26, ((r1 >> 27) & 3) << 26); r0 = 0; }
void vita_hle_sceGxmTextureGetStride(void)    { r0 = 0; }
void vita_hle_sceGxmTextureSetStride(void)    { r0 = 0; }
void vita_hle_sceGxmTextureGetNormalizeMode(void) { r0 = (tw(r0, 3) >> 31) & 1; }
void vita_hle_sceGxmTextureSetNormalizeMode(void) { tset(r0, 3, 1u << 31, (r1 ? 1u : 0u) << 31); r0 = 0; }
void vita_hle_sceGxmTextureValidate(void) { r0 = 0; }

/* --- the GXT texture container and transfers: later --------------------------------------- */

void vita_hle_sceGxmTransferCopy(void)     { r0 = 0; }
void vita_hle_sceGxmTransferDownscale(void){ r0 = 0; }
void vita_hle_sceGxmTransferFill(void)     { r0 = 0; }
void vita_hle_sceGxmTransferFinish(void)   { r0 = 0; }
