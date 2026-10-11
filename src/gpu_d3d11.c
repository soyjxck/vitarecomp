/* gpu_d3d11.c — gpu.h on Direct3D 11.
 *
 * Nothing here is clever. A surface is a texture with the views it can be
 * drawn to and read through; a draw is its buffers copied into buffers
 * that grow, its state looked up among the state objects already made, and
 * one call. The shader compiler is the system's (d3dcompiler),
 * found at run time like the rest of Direct3D: nothing is added to what a
 * program built with this ships.
 */

#include "gpu.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONSTANT_BYTES 1232u            /* gxp_hlsl.h's cbuffer: 66 + 3 + 8 rows of sixteen bytes */
#define KEPT 64                         /* state objects of a kind */
#define QUERIES 4096
#define READERS 24

struct gpu_shader { ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *layout; };
struct gpu_texture {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *view;
    ID3D11RenderTargetView *target;         /* of one gpu_convert fills */
    uint32_t width, height;
};

struct gpu_surface {
    unsigned format;
    uint32_t width, height;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *target;         /* a colour's */
    ID3D11UnorderedAccessView *ordered;     /* and, where the card can, for a program to read and write itself */
    ID3D11DepthStencilView *depth;          /* a depth's */
    ID3D11ShaderResourceView *view, *view2; /* to read it as a texture: a colour, or a depth and its stencil */
    ID3D11Texture2D *copy;                  /* a colour as it stood before a draw, for a program that reads it: made when first wanted */
    ID3D11ShaderResourceView *copy_view;
};

typedef struct { uint8_t key[24]; void *object; } kept;

static struct {
    int opened, ok;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    char name[200];

    /* the surfaces of the list in hand */
    gpu_surface *colour, *depth;
    int in_list, failed;
    int ordered;                                    /* the card has rasteriser-ordered views of more than one word */
    /* textures the processor can read, for taking a surface back: one of each form and size met */
    struct { DXGI_FORMAT format; uint32_t width, height; ID3D11Texture2D *tex; } readers[READERS];
    unsigned reader_count;
    /* for gpu_convert: three corners that cover a target, and no tests */
    ID3D11VertexShader *whole;
    ID3D11DepthStencilState *untested;
    /* for gpu_surface_depth_corner: a program that hands a depth on, and every depth written */
    ID3D11PixelShader *depth_on;
    ID3D11DepthStencilState *all_written;

    ID3D11Buffer *vertices, *indices, *constants[2], *memory[2];
    ID3D11ShaderResourceView *memory_view[2];
    uint32_t vertices_cap, indices_cap, memory_cap[2];

    kept rasters[4], depths[KEPT], blends[KEPT], samplers[KEPT];
    unsigned raster_count, depth_count, blend_count, sampler_count;

    ID3D11Query *queries[QUERIES];
    unsigned query_count;
} G;

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static DXGI_FORMAT format_of(unsigned f) {
    switch (f) {
    case GPU_RGBA8:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GPU_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case GPU_BGRA8:      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GPU_BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case GPU_RGBA16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GPU_RGBA32F:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GPU_R32UI:      return DXGI_FORMAT_R32_UINT;
    case GPU_RG32UI:     return DXGI_FORMAT_R32G32_UINT;
    case GPU_RGBA32UI:   return DXGI_FORMAT_R32G32B32A32_UINT;
    case GPU_D16:        return DXGI_FORMAT_R16_TYPELESS;
    case GPU_D24S8:      return DXGI_FORMAT_R24G8_TYPELESS;
    case GPU_D32FS8:     return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:             return DXGI_FORMAT_UNKNOWN;
    }
}

static DXGI_FORMAT depth_view_format(unsigned f) {
    return f == GPU_D16 ? DXGI_FORMAT_D16_UNORM : f == GPU_D24S8 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

/* A surface's texture has no one form where it is read in two: bytes of
 * colour are drawn on the display's curve or off it and read as bytes
 * either way; depth is drawn as depth and read as numbers. So the texture
 * is made of a family and each view names its member. */
static DXGI_FORMAT surface_family(unsigned f) {
    switch (f) {
    case GPU_RGBA8: case GPU_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case GPU_BGRA8: case GPU_BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default:                             return format_of(f);
    }
}

static DXGI_FORMAT surface_read_format(unsigned f) {
    switch (f) {
    case GPU_RGBA8: case GPU_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GPU_BGRA8: case GPU_BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GPU_D16:                        return DXGI_FORMAT_R16_UNORM;
    case GPU_D24S8:                      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case GPU_D32FS8:                     return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:                             return format_of(f);
    }
}

static DXGI_FORMAT surface_stencil_format(unsigned f) {
    return f == GPU_D24S8 ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : f == GPU_D32FS8 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_UNKNOWN;
}

static unsigned texel_bytes(unsigned f) {
    switch (f) {
    case GPU_RGBA16F: case GPU_RG32UI: case GPU_D32FS8: return 8;
    case GPU_RGBA32F: case GPU_RGBA32UI: return 16;
    case GPU_D16: return 2;
    default: return 4;
    }
}

/* --- the card ------------------------------------------------------------------------------- */

int gpu_open(void) {
    if (G.opened) return G.ok ? 0 : -1;
    G.opened = 1;
    const char *warp = getenv("VITARECOMP_GPU_WARP"), *debug = getenv("VITARECOMP_GPU_DEBUG");
    static const D3D_FEATURE_LEVEL want[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    UINT flags = debug && *debug && *debug != '0' ? D3D11_CREATE_DEVICE_DEBUG : 0;
    HRESULT hr = E_FAIL;
    /* VITARECOMP_GPU_WARP=1: the system's own rasteriser, which is everywhere and slow: for tests */
    if (!(warp && *warp && *warp != '0'))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, want, 2, D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags, want, 2, D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
    if (FAILED(hr)) return -1;

    snprintf(G.name, sizeof(G.name), "Direct3D 11");
    IDXGIDevice *dxgi = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(G.dev, &IID_IDXGIDevice, (void **)&dxgi))) {
        IDXGIAdapter *adapter = NULL;
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter))) {
            DXGI_ADAPTER_DESC desc;
            char card[128] = "";
            if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc)))
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, card, sizeof(card), NULL, NULL);
            snprintf(G.name, sizeof(G.name), "Direct3D 11 on %s", card);
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi);
    }

    for (int k = 0; k < 2; k++) {
        D3D11_BUFFER_DESC d;
        memset(&d, 0, sizeof(d));
        d.ByteWidth = CONSTANT_BYTES;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(G.dev, &d, NULL, &G.constants[k]))) return -1;
    }
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 more;
        memset(&more, 0, sizeof(more));
        if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(G.dev, D3D11_FEATURE_D3D11_OPTIONS2, &more, sizeof(more))))
            G.ordered = more.ROVsSupported && more.TypedUAVLoadAdditionalFormats;
    }
    G.ok = 1;
    return 0;
}

int gpu_ordered(unsigned format) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 can;
    const UINT both = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if (!G.ok || !G.ordered) return 0;
    /* four numbers a pixel, and not kept on the display's curve: a view of that sort has no curve */
    if (format != GPU_RGBA8 && format != GPU_RGBA16F && format != GPU_RGBA32F) return 0;
    memset(&can, 0, sizeof(can));
    can.InFormat = format_of(format);
    return SUCCEEDED(ID3D11Device_CheckFeatureSupport(G.dev, D3D11_FEATURE_FORMAT_SUPPORT2, &can, sizeof(can))) &&
           (can.OutFormatSupport2 & both) == both;
}

const char *gpu_name(void) { return G.ok ? G.name : "nothing"; }

/* --- shaders --------------------------------------------------------------------------------- */

gpu_shader *gpu_shader_make(int fragment, const char *hlsl, unsigned pa_words, char *log, size_t log_size) {
    ID3DBlob *code = NULL, *errors = NULL;
    if (log && log_size) log[0] = '\0';
    if (!G.ok) return NULL;
    /* the translation leans on the arithmetic being done as written: a NaN test must stay a test */
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), NULL, NULL, NULL, "main", fragment ? "ps_5_0" : "vs_5_0",
                            D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(hr)) {
        if (log && log_size && errors)
            snprintf(log, log_size, "%.*s", (int)ID3D10Blob_GetBufferSize(errors), (const char *)ID3D10Blob_GetBufferPointer(errors));
        RELEASE(errors);
        return NULL;
    }
    RELEASE(errors);
    gpu_shader *s = calloc(1, sizeof(*s));
    const void *bytes = ID3D10Blob_GetBufferPointer(code);
    SIZE_T size = ID3D10Blob_GetBufferSize(code);
    if (s && fragment) {
        hr = ID3D11Device_CreatePixelShader(G.dev, bytes, size, NULL, &s->ps);
    } else if (s) {
        D3D11_INPUT_ELEMENT_DESC in[16];
        unsigned n = pa_words / 4;
        if (n > 16) n = 16;
        memset(in, 0, sizeof(in));
        for (unsigned k = 0; k < n; k++) {
            in[k].SemanticName = "PA";
            in[k].SemanticIndex = k;
            in[k].Format = DXGI_FORMAT_R32G32B32A32_UINT;
            in[k].AlignedByteOffset = k * 16;
            in[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        }
        if (!n) {                           /* a program fed nothing still declares an input */
            in[0].SemanticName = "PA";
            in[0].Format = DXGI_FORMAT_R32_UINT;
            in[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            n = 1;
        }
        hr = ID3D11Device_CreateVertexShader(G.dev, bytes, size, NULL, &s->vs);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(G.dev, in, n, bytes, size, &s->layout);
    }
    RELEASE(code);
    if (!s || FAILED(hr)) {
        if (log && log_size) snprintf(log, log_size, "the card refused the compiled program (0x%08lX)", (unsigned long)hr);
        gpu_shader_free(s);
        return NULL;
    }
    return s;
}

void gpu_shader_free(gpu_shader *s) {
    if (!s) return;
    RELEASE(s->vs);
    RELEASE(s->ps);
    RELEASE(s->layout);
    free(s);
}

/* --- textures --------------------------------------------------------------------------------- */

gpu_texture *gpu_texture_make(unsigned format, unsigned width, unsigned height, unsigned levels, int cube,
                              const void *const *images) {
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init[6 * 16];
    unsigned faces = cube ? 6 : 1, bytes = texel_bytes(format);
    if (!G.ok || !width || !height || !levels || levels > 16 || format_of(format) == DXGI_FORMAT_UNKNOWN) return NULL;
    memset(&d, 0, sizeof(d));
    d.Width = width;
    d.Height = height;
    d.MipLevels = levels;
    d.ArraySize = faces;
    d.Format = format_of(format);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = 0;                        /* a cube is six faces in a row, sampled as such (gxp_hlsl.c's cube_at) */
    /* the card numbers a face's levels together; the images come a level's faces together */
    for (unsigned level = 0; level < levels; level++) {
        unsigned w = width >> level ? width >> level : 1;
        for (unsigned face = 0; face < faces; face++) {
            D3D11_SUBRESOURCE_DATA *at = &init[face * levels + level];
            at->pSysMem = images[level * faces + face];
            at->SysMemPitch = w * bytes;
            at->SysMemSlicePitch = 0;
            if (!at->pSysMem) return NULL;
        }
    }
    gpu_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &d, init, &t->tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)t->tex, NULL, &t->view))) {
        gpu_texture_free(t);
        return NULL;
    }
    return t;
}

gpu_texture *gpu_texture_blank(unsigned width, unsigned height) {
    D3D11_TEXTURE2D_DESC d;
    if (!G.ok || !width || !height) return NULL;
    memset(&d, 0, sizeof(d));
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    gpu_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->width = width;
    t->height = height;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &d, NULL, &t->tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)t->tex, NULL, &t->view)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)t->tex, NULL, &t->target))) {
        gpu_texture_free(t);
        return NULL;
    }
    return t;
}

void gpu_texture_free(gpu_texture *t) {
    if (!t) return;
    RELEASE(t->target);
    RELEASE(t->view);
    RELEASE(t->tex);
    free(t);
}

/* --- buffers that grow -------------------------------------------------------------------------- */

static int room(ID3D11Buffer **buffer, uint32_t *cap, uint32_t bytes, UINT bind, ID3D11ShaderResourceView **view) {
    if (*buffer && *cap >= bytes) return 1;
    uint32_t grown = *cap ? *cap * 2 : 1u << 16;
    while (grown < bytes) grown *= 2;
    D3D11_BUFFER_DESC d;
    memset(&d, 0, sizeof(d));
    d.ByteWidth = grown;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = bind;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (view) RELEASE(*view);
    RELEASE(*buffer);
    *cap = 0;
    if (FAILED(ID3D11Device_CreateBuffer(G.dev, &d, NULL, buffer))) return 0;
    if (view) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof(v));
        v.Format = DXGI_FORMAT_R32_UINT;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.FirstElement = 0;
        v.Buffer.NumElements = grown / 4;
        if (FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)*buffer, &v, view))) return 0;
    }
    *cap = grown;
    return 1;
}

static int fill(ID3D11Buffer *buffer, const void *data, uint32_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)buffer, 0);
    return 1;
}

/* --- state, made once of each kind met ---------------------------------------------------------- */

static void *find(kept *table, unsigned count, const uint8_t key[24]) {
    for (unsigned i = 0; i < count; i++)
        if (!memcmp(table[i].key, key, 24)) return table[i].object;
    return NULL;
}

static void keep(kept *table, unsigned *count, unsigned cap, const uint8_t key[24], void *object) {
    if (*count == cap) {                    /* full: the oldest goes */
        RELEASE(table[0].object);
        memmove(table, table + 1, (cap - 1) * sizeof(*table));
        (*count)--;
    }
    memcpy(table[*count].key, key, 24);
    table[(*count)++].object = object;
}

static ID3D11RasterizerState *raster_state(unsigned cull) {
    uint8_t key[24] = { 0 };
    key[0] = (uint8_t)cull;
    ID3D11RasterizerState *s = find(G.rasters, G.raster_count, key);
    if (s) return s;
    D3D11_RASTERIZER_DESC d;
    memset(&d, 0, sizeof(d));
    d.FillMode = D3D11_FILL_SOLID;
    /* counter-clockwise on the screen is the front */
    d.CullMode = cull == 1 ? D3D11_CULL_BACK : cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    d.FrontCounterClockwise = TRUE;
    d.DepthClipEnable = FALSE;              /* nearer than the near plane is drawn; the far end is cut by the program */
    d.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(G.dev, &d, &s))) return NULL;
    keep(G.rasters, &G.raster_count, 4, key, s);
    return s;
}

static ID3D11DepthStencilState *depth_state(const gpu_draw_call *c) {
    uint8_t key[24] = { 0 };
    key[0] = c->depth_func; key[1] = c->depth_write; key[2] = c->stencil_read; key[3] = c->stencil_write;
    memcpy(key + 4, c->stencil, sizeof(c->stencil));
    ID3D11DepthStencilState *s = find(G.depths, G.depth_count, key);
    if (s) return s;
    D3D11_DEPTH_STENCIL_DESC d;
    memset(&d, 0, sizeof(d));
    d.DepthEnable = TRUE;
    d.DepthWriteMask = c->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = (D3D11_COMPARISON_FUNC)((c->depth_func & 7) + 1);     /* both number them never to always */
    d.StencilEnable = TRUE;
    d.StencilReadMask = c->stencil_read;
    d.StencilWriteMask = c->stencil_write;
    D3D11_DEPTH_STENCILOP_DESC *face[2] = { &d.FrontFace, &d.BackFace };
    for (int k = 0; k < 2; k++) {
        face[k]->StencilFunc = (D3D11_COMPARISON_FUNC)((c->stencil[k].func & 7) + 1);
        face[k]->StencilFailOp = (D3D11_STENCIL_OP)((c->stencil[k].fail & 7) + 1);   /* and the operations keep to wrap */
        face[k]->StencilDepthFailOp = (D3D11_STENCIL_OP)((c->stencil[k].depth_fail & 7) + 1);
        face[k]->StencilPassOp = (D3D11_STENCIL_OP)((c->stencil[k].pass & 7) + 1);
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(G.dev, &d, &s))) return NULL;
    keep(G.depths, &G.depth_count, KEPT, key, s);
    return s;
}

static D3D11_BLEND factor(unsigned f, int alpha) {
    switch (f) {
    case 0:  return D3D11_BLEND_ZERO;
    case 1:  return D3D11_BLEND_ONE;
    case 2:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 3:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 4:  return D3D11_BLEND_SRC_ALPHA;
    case 5:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 6:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 7:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 8:  return D3D11_BLEND_DEST_ALPHA;
    case 9:  return D3D11_BLEND_INV_DEST_ALPHA;
    default: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    }
}

static D3D11_BLEND_OP operation(unsigned op) {
    /* the card's "subtract" takes the source from the destination */
    return op == 2 ? D3D11_BLEND_OP_REV_SUBTRACT : op == 3 ? D3D11_BLEND_OP_SUBTRACT : op == 4 ? D3D11_BLEND_OP_MIN
         : op == 5 ? D3D11_BLEND_OP_MAX : D3D11_BLEND_OP_ADD;
}

static ID3D11BlendState *blend_state(const gpu_draw_call *c) {
    uint8_t key[24] = { 0 };
    key[0] = c->blend; key[1] = c->colour_op; key[2] = c->alpha_op; key[3] = c->colour_src; key[4] = c->colour_dst;
    key[5] = c->alpha_src; key[6] = c->alpha_dst; key[7] = c->write_mask;
    ID3D11BlendState *s = find(G.blends, G.blend_count, key);
    if (s) return s;
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof(d));
    D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
    r->BlendEnable = c->blend ? TRUE : FALSE;
    /* an operation of 0 on one of the two is the source as it is */
    r->BlendOp = operation(c->colour_op);
    r->SrcBlend = c->colour_op ? factor(c->colour_src, 0) : D3D11_BLEND_ONE;
    r->DestBlend = c->colour_op ? factor(c->colour_dst, 0) : D3D11_BLEND_ZERO;
    r->BlendOpAlpha = operation(c->alpha_op);
    r->SrcBlendAlpha = c->alpha_op ? factor(c->alpha_src, 1) : D3D11_BLEND_ONE;
    r->DestBlendAlpha = c->alpha_op ? factor(c->alpha_dst, 1) : D3D11_BLEND_ZERO;
    r->RenderTargetWriteMask = c->write_mask & 15;
    if (FAILED(ID3D11Device_CreateBlendState(G.dev, &d, &s))) return NULL;
    keep(G.blends, &G.blend_count, KEPT, key, s);
    return s;
}

static ID3D11SamplerState *sampler_state(const gpu_sampler *g) {
    static const D3D11_TEXTURE_ADDRESS_MODE ADDRESS[4] = {
        D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE };
    uint8_t key[24] = { 0 };
    key[0] = g->min_linear; key[1] = g->mag_linear; key[2] = g->mip; key[3] = g->address_u; key[4] = g->address_v;
    memcpy(key + 8, &g->lod_bias, 4);
    memcpy(key + 12, &g->lod_min, 4);
    memcpy(key + 16, &g->lod_max, 4);
    ID3D11SamplerState *s = find(G.samplers, G.sampler_count, key);
    if (s) return s;
    D3D11_SAMPLER_DESC d;
    memset(&d, 0, sizeof(d));
    d.Filter = (D3D11_FILTER)((g->min_linear ? 0x10 : 0) | (g->mag_linear ? 0x4 : 0) | (g->mip == 2 ? 0x1 : 0));
    d.AddressU = ADDRESS[g->address_u & 3];
    d.AddressV = ADDRESS[g->address_v & 3];
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MipLODBias = g->lod_bias;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MinLOD = g->lod_min;
    d.MaxLOD = g->mip ? g->lod_max : g->lod_min;
    if (FAILED(ID3D11Device_CreateSamplerState(G.dev, &d, &s))) return NULL;
    keep(G.samplers, &G.sampler_count, KEPT, key, s);
    return s;
}

/* --- surfaces ----------------------------------------------------------------------------------- */

static int is_depth(unsigned format) { return format == GPU_D16 || format == GPU_D24S8 || format == GPU_D32FS8; }

/* The texture of a surface and what it is drawn through; with `rows`, made
 * holding them. A depth texture is made anew to be filled: the card takes
 * depth and stencil together only as a texture's first contents. */
static int surface_texture(gpu_surface *s, const void *rows, uint32_t pitch) {
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_SHADER_RESOURCE_VIEW_DESC read;
    memset(&d, 0, sizeof(d));
    d.Width = s->width;
    d.Height = s->height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Format = surface_family(s->format);
    d.Usage = D3D11_USAGE_DEFAULT;
    init.pSysMem = rows;
    init.SysMemPitch = pitch;
    init.SysMemSlicePitch = 0;
    memset(&read, 0, sizeof(read));
    read.Format = surface_read_format(s->format);
    read.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    read.Texture2D.MipLevels = 1;
    RELEASE(s->target); RELEASE(s->ordered); RELEASE(s->depth); RELEASE(s->view); RELEASE(s->view2); RELEASE(s->tex);
    if (is_depth(s->format)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC v;
        memset(&v, 0, sizeof(v));
        v.Format = depth_view_format(s->format);
        v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &d, rows ? &init : NULL, &s->tex)) ||
            FAILED(ID3D11Device_CreateDepthStencilView(G.dev, (ID3D11Resource *)s->tex, &v, &s->depth)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)s->tex, &read, &s->view))) return 0;
        read.Format = surface_stencil_format(s->format);
        return read.Format == DXGI_FORMAT_UNKNOWN ||
               SUCCEEDED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)s->tex, &read, &s->view2));
    }
    D3D11_RENDER_TARGET_VIEW_DESC draw;
    int ordered = gpu_ordered(s->format);
    memset(&draw, 0, sizeof(draw));
    draw.Format = format_of(s->format);
    draw.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (ordered ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &d, rows ? &init : NULL, &s->tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)s->tex, &draw, &s->target)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)s->tex, &read, &s->view))) return 0;
    if (ordered) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC both;
        memset(&both, 0, sizeof(both));
        both.Format = format_of(s->format);
        both.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(G.dev, (ID3D11Resource *)s->tex, &both, &s->ordered))) s->ordered = NULL;
    }
    return 1;
}

gpu_surface *gpu_surface_make(unsigned format, uint32_t width, uint32_t height) {
    if (!G.ok || !width || !height || format_of(format) == DXGI_FORMAT_UNKNOWN) return NULL;
    gpu_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->format = format;
    s->width = width;
    s->height = height;
    if (!surface_texture(s, NULL, 0)) { gpu_surface_free(s); return NULL; }
    return s;
}

void gpu_surface_free(gpu_surface *s) {
    if (!s) return;
    if (G.colour == s) G.colour = NULL;
    if (G.depth == s) G.depth = NULL;
    RELEASE(s->target); RELEASE(s->ordered); RELEASE(s->depth); RELEASE(s->view); RELEASE(s->view2);
    RELEASE(s->copy_view); RELEASE(s->copy); RELEASE(s->tex);
    free(s);
}

int gpu_surface_put(gpu_surface *s, const void *rows, uint32_t pitch) {
    if (!s || !rows || G.in_list) return -1;
    if (is_depth(s->format)) return surface_texture(s, rows, pitch) ? 0 : -1;
    ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)s->tex, 0, NULL, rows, pitch, 0);
    return 0;
}

/* A texture the processor can map, of a surface's form and size. */
static ID3D11Texture2D *reader_for(const gpu_surface *s) {
    DXGI_FORMAT format = is_depth(s->format) ? format_of(s->format) : surface_read_format(s->format);
    for (unsigned i = 0; i < G.reader_count; i++)
        if (G.readers[i].format == format && G.readers[i].width == s->width && G.readers[i].height == s->height) return G.readers[i].tex;
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof(d));
    d.Width = s->width;
    d.Height = s->height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Format = format;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (G.reader_count == READERS) {                /* full: the oldest goes */
        RELEASE(G.readers[0].tex);
        memmove(G.readers, G.readers + 1, (READERS - 1) * sizeof(G.readers[0]));
        G.reader_count--;
    }
    ID3D11Texture2D *tex = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &d, NULL, &tex))) return NULL;
    G.readers[G.reader_count].format = format;
    G.readers[G.reader_count].width = s->width;
    G.readers[G.reader_count].height = s->height;
    G.readers[G.reader_count++].tex = tex;
    return tex;
}

int gpu_surface_get(gpu_surface *s, void *rows, uint32_t pitch) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (!s || !rows || G.in_list) return -1;
    ID3D11Texture2D *reader = reader_for(s);
    uint32_t row_bytes = s->width * texel_bytes(s->format);
    if (!reader) return -1;
    ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)reader, (ID3D11Resource *)s->tex);
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)reader, 0, D3D11_MAP_READ, 0, &m))) return -1;
    for (uint32_t y = 0; y < s->height; y++)
        memcpy((uint8_t *)rows + (size_t)y * pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, row_bytes);
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)reader, 0);
    return 0;
}

void gpu_surface_clear(gpu_surface *s, float depth, uint8_t stencil) {
    if (s && s->depth) ID3D11DeviceContext_ClearDepthStencilView(G.ctx, s->depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, stencil);
}

int gpu_surface_copy(gpu_surface *into, gpu_surface *from) {
    if (!into || !from || G.in_list || into->format != from->format || into->width != from->width || into->height != from->height) return -1;
    ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)into->tex, (ID3D11Resource *)from->tex);
    return 0;
}

int gpu_surface_copy_corner(gpu_surface *into, gpu_surface *from, uint32_t width, uint32_t height) {
    if (!into || !from || G.in_list || into->format != from->format || is_depth(into->format) || width > into->width || height > into->height ||
        width > from->width || height > from->height) return -1;
    D3D11_BOX box = { 0, 0, 0, width, height, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(G.ctx, (ID3D11Resource *)into->tex, 0, 0, 0, 0, (ID3D11Resource *)from->tex, 0, &box);
    return 0;
}

/* --- a texture from a surface ------------------------------------------------------------------- */

/* Three corners that cover whatever is drawn to. */
static int whole_made(void) {
    static const char WHOLE[] =
        "float4 main(uint n : SV_VertexID) : SV_Position { return float4(float((n << 1) & 2u) * 2.0 - 1.0, 1.0 - float(n & 2u) * 2.0, 0.0, 1.0); }\n";
    if (G.whole) return 1;
    ID3DBlob *code = NULL;
    if (FAILED(D3DCompile(WHOLE, sizeof(WHOLE) - 1, NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &code, NULL))) return 0;
    HRESULT hr = ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &G.whole);
    RELEASE(code);
    return SUCCEEDED(hr);
}

int gpu_surface_depth_corner(gpu_surface *into, gpu_surface *from, uint8_t stencil) {
    static const char ON[] = "Texture2D<float> SRC : register(t0);\nfloat main(float4 pos : SV_Position) : SV_Depth { return SRC.Load(int3(pos.xy, 0)); }\n";
    static const float NO_FACTOR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (!G.ok || G.in_list || !into || !from || !into->depth || !from->view || into->format != from->format ||
        into->width > from->width || into->height > from->height || !whole_made()) return -1;
    if (!G.depth_on) {
        ID3DBlob *code = NULL;
        if (FAILED(D3DCompile(ON, sizeof(ON) - 1, NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &code, NULL))) return -1;
        HRESULT hr = ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &G.depth_on);
        RELEASE(code);
        if (FAILED(hr)) return -1;
    }
    if (!G.all_written) {
        D3D11_DEPTH_STENCIL_DESC d;
        memset(&d, 0, sizeof(d));
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(ID3D11Device_CreateDepthStencilState(G.dev, &d, &G.all_written))) return -1;
    }
    ID3D11RasterizerState *raster = raster_state(0);
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT port = { 0.0f, 0.0f, (float)into->width, (float)into->height, 0.0f, 1.0f };
    D3D11_RECT all = { 0, 0, (LONG)into->width, (LONG)into->height };
    if (!raster) return -1;
    ID3D11DeviceContext_ClearDepthStencilView(G.ctx, into->depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, stencil);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, into->depth);
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &port);
    ID3D11DeviceContext_RSSetScissorRects(G.ctx, 1, &all);
    ID3D11DeviceContext_RSSetState(G.ctx, raster);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, G.all_written, 0);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, NULL, NO_FACTOR, 0xFFFFFFFFu);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.whole, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx, G.depth_on, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 1, &from->view);
    ID3D11DeviceContext_Draw(G.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    return 0;
}

int gpu_convert(gpu_texture *into, gpu_surface *from, gpu_shader *program, const void *constants, uint32_t constants_bytes) {
    if (!G.ok || G.in_list || !into || !into->target || !from || !from->view || !program || !program->ps || !whole_made()) return -1;
    if (!G.untested) {
        D3D11_DEPTH_STENCIL_DESC d;
        memset(&d, 0, sizeof(d));
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(ID3D11Device_CreateDepthStencilState(G.dev, &d, &G.untested))) return -1;
    }
    ID3D11RasterizerState *raster = raster_state(0);
    uint8_t block[CONSTANT_BYTES];
    if (!raster) return -1;
    memset(block, 0, sizeof(block));
    if (constants) memcpy(block, constants, constants_bytes < 64 ? constants_bytes : 64);
    if (!fill(G.constants[1], block, CONSTANT_BYTES)) return -1;

    static const float NO_FACTOR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11ShaderResourceView *views[2] = { from->view, from->view2 }, *none[2] = { NULL, NULL };
    D3D11_VIEWPORT port = { 0.0f, 0.0f, (float)into->width, (float)into->height, 0.0f, 1.0f };
    D3D11_RECT all = { 0, 0, (LONG)into->width, (LONG)into->height };
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &into->target, NULL);
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &port);
    ID3D11DeviceContext_RSSetScissorRects(G.ctx, 1, &all);
    ID3D11DeviceContext_RSSetState(G.ctx, raster);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, G.untested, 0);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, NULL, NO_FACTOR, 0xFFFFFFFFu);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.whole, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx, program->ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.constants[1]);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 2, views);
    ID3D11DeviceContext_Draw(G.ctx, 3, 0);
    /* neither stays bound: the texture is about to be sampled and the surface drawn to */
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    return 0;
}

/* --- a list ------------------------------------------------------------------------------------- */

int gpu_begin(gpu_surface *colour, gpu_surface *depth) {
    if (!G.ok || G.in_list || !depth || !depth->depth) return -1;
    if (colour && (!colour->target || colour->width != depth->width || colour->height != depth->height)) return -1;
    D3D11_VIEWPORT port = { 0.0f, 0.0f, (float)depth->width, (float)depth->height, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &port);
    G.colour = colour;
    G.depth = depth;
    G.in_list = 1;
    G.failed = 0;
    G.query_count = 0;
    return 0;
}

int gpu_draw(const gpu_draw_call *c) {
    static const float NO_FACTOR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int counted = -1;
    if (!G.in_list || !c->vertex || !c->vertex->vs || !c->index_count || !c->vertex_count) return -1;

    ID3D11RasterizerState *raster = raster_state(c->cull);
    ID3D11DepthStencilState *depth = depth_state(c);
    ID3D11BlendState *blend = blend_state(c);
    if (!raster || !depth || !blend) { G.failed = 1; return -1; }

    uint32_t vertex_bytes = c->vertex_count * c->vertex_stride, index_bytes = c->index_count * c->index_size;
    if (!room(&G.vertices, &G.vertices_cap, vertex_bytes ? vertex_bytes : 4, D3D11_BIND_VERTEX_BUFFER, NULL) ||
        !room(&G.indices, &G.indices_cap, index_bytes, D3D11_BIND_INDEX_BUFFER, NULL) ||
        (vertex_bytes && !fill(G.vertices, c->vertices, vertex_bytes)) || !fill(G.indices, c->indices, index_bytes)) { G.failed = 1; return -1; }

    for (int k = 0; k < 2; k++) {
        uint8_t block[CONSTANT_BYTES];
        ID3D11ShaderResourceView *views[16], *words = NULL;
        ID3D11SamplerState *samplers[16];
        uint32_t n = c->constants_bytes[k] < CONSTANT_BYTES ? c->constants_bytes[k] : CONSTANT_BYTES;
        memset(block, 0, sizeof(block));
        if (c->constants[k]) memcpy(block, c->constants[k], n);
        if (!fill(G.constants[k], block, CONSTANT_BYTES)) { G.failed = 1; return -1; }
        if (c->memory[k] && c->memory_words[k]) {
            if (!room(&G.memory[k], &G.memory_cap[k], c->memory_words[k] * 4, D3D11_BIND_SHADER_RESOURCE, &G.memory_view[k]) ||
                !fill(G.memory[k], c->memory[k], c->memory_words[k] * 4)) { G.failed = 1; return -1; }
            words = G.memory_view[k];
        }
        for (int unit = 0; unit < 16; unit++) {
            views[unit] = c->textures[k][unit] ? c->textures[k][unit]->view : NULL;
            samplers[unit] = c->textures[k][unit] ? sampler_state(&c->samplers[k][unit]) : NULL;
        }
        if (k == 0) {
            ID3D11DeviceContext_VSSetConstantBuffers(G.ctx, 0, 1, &G.constants[0]);
            ID3D11DeviceContext_VSSetShaderResources(G.ctx, 0, 16, views);
            ID3D11DeviceContext_VSSetSamplers(G.ctx, 0, 16, samplers);
            ID3D11DeviceContext_VSSetShaderResources(G.ctx, 16, 1, &words);
        } else {
            ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.constants[1]);
            ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 16, views);
            ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 16, samplers);
            ID3D11DeviceContext_PSSetShaderResources(G.ctx, 16, 1, &words);
        }
    }

    /* a program that reads the pixel it writes is handed the target itself, to read and
     * write in the order its triangles come; or, where that cannot be, a copy made before
     * the draw, and the target as any other program has it */
    ID3D11ShaderResourceView *now = NULL;
    gpu_surface *colour = G.colour;
    int ordered = c->reads_pixel == 2 && colour && colour->ordered && c->fragment;
    if (c->reads_pixel == 2 && !ordered) { G.failed = 1; return -1; }
    if (c->reads_pixel && !ordered && colour && c->fragment) {
        if (!colour->copy) {
            D3D11_TEXTURE2D_DESC d;
            D3D11_SHADER_RESOURCE_VIEW_DESC as_drawn;
            ID3D11Texture2D_GetDesc(colour->tex, &d);
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            memset(&as_drawn, 0, sizeof(as_drawn));
            as_drawn.Format = format_of(colour->format);
            as_drawn.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            as_drawn.Texture2D.MipLevels = 1;
            if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &d, NULL, &colour->copy)) ||
                FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)colour->copy, &as_drawn, &colour->copy_view))) {
                RELEASE(colour->copy);
                G.failed = 1;
                return -1;
            }
        }
        ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)colour->copy, (ID3D11Resource *)colour->tex);
        now = colour->copy_view;
    }
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 17, 1, &now);
    if (ordered)
        ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(G.ctx, 0, NULL, G.depth->depth, 0, 1, &colour->ordered, NULL);
    else
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, colour ? 1 : 0, colour ? &colour->target : NULL, G.depth->depth);

    UINT stride = c->vertex_stride, offset = 0;
    D3D11_RECT scissor = { c->scissor[0], c->scissor[1], c->scissor[2], c->scissor[3] };
    ID3D11DeviceContext_IASetInputLayout(G.ctx, c->vertex->layout);
    ID3D11DeviceContext_IASetVertexBuffers(G.ctx, 0, 1, &G.vertices, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(G.ctx, G.indices, c->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, c->strip ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, c->vertex->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx, c->fragment ? c->fragment->ps : NULL, NULL, 0);
    ID3D11DeviceContext_RSSetState(G.ctx, raster);
    ID3D11DeviceContext_RSSetScissorRects(G.ctx, 1, &scissor);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, depth, c->stencil_ref);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, blend, NO_FACTOR, 0xFFFFFFFFu);

    ID3D11Query *query = NULL;
    if (c->count && G.query_count < QUERIES) {
        D3D11_QUERY_DESC q = { D3D11_QUERY_OCCLUSION, 0 };
        if (SUCCEEDED(ID3D11Device_CreateQuery(G.dev, &q, &query))) {
            counted = (int)G.query_count;
            G.queries[G.query_count++] = query;
            ID3D11DeviceContext_Begin(G.ctx, (ID3D11Asynchronous *)query);
        }
    }
    ID3D11DeviceContext_DrawIndexed(G.ctx, c->index_count, 0, 0);
    if (query) ID3D11DeviceContext_End(G.ctx, (ID3D11Asynchronous *)query);
    return counted;
}

int gpu_end(uint64_t *counts, unsigned max_counts) {
    int ok = !G.failed;
    if (!G.in_list) return -1;
    G.in_list = 0;
    /* nothing of the list stays bound: its surfaces may be read as textures by the next */
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    for (unsigned i = 0; i < G.query_count; i++) {
        UINT64 passed = 0;
        while (ID3D11DeviceContext_GetData(G.ctx, (ID3D11Asynchronous *)G.queries[i], &passed, sizeof(passed), 0) == S_FALSE) Sleep(0);
        if (counts && i < max_counts) counts[i] = passed;
        RELEASE(G.queries[i]);
    }
    G.query_count = 0;
    return ok ? 0 : -1;
}
