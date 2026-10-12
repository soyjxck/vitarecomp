/* gpu_gl.c — gpu.h on OpenGL.
 *
 * The same page as gpu_d3d11.c in another library's terms. A surface is a
 * texture; a list is drawn through a frame buffer its two surfaces are
 * attached to; a draw is its vertices and its constants added to the end
 * of buffers that are written a piece at a time, its state set where it
 * is not the last draw's, and one call.
 *
 * Which OpenGL: the desktop's, version 4.3, the core of it. The Nintendo
 * Switch's library (Mesa, by way of EGL) offers that and the one for small
 * devices, version 3.2, and the small one has not what this interface
 * asks: a count of the pixels a draw passed (it says only whether any
 * did), depth that is held to the near plane and not cut by it, a plane
 * of the program's own to cut by, clip space read the way the programs
 * leave it, textures of floats that are filtered and drawn to without an
 * extension apiece; and its language converts nothing by itself, where
 * the translator's text (gxp_hlsl.h) leans on a whole number becoming an
 * unsigned one. 4.3 has all of it but one thing, and on Windows it is
 * what every card's driver has had for ten years.
 *
 * The one thing: a fragment program reading and writing its target a
 * pixel at a time in the order the triangles come (gpu_ordered). OpenGL
 * has no such thing short of an extension the Switch's card does not
 * offer, so the answer is no, and a program that reads the pixel it
 * writes is given the target as it stood before its draw: gpu.h's answer
 * for a card without the means.
 *
 * Rows. Direct3D counts a picture's rows from the top and OpenGL from the
 * bottom, and everything above this file, the programs included, counts
 * from the top. So here a texture's first row is the picture's top row
 * whatever the library calls it, in what is put, what is got and what is
 * drawn; the card is told to read clip space as Direct3D does
 * (glClipControl: up is the first row, depth from nothing to one), which
 * also makes a pixel that lies exactly on an edge fall to the same side;
 * and only the window, which is the library's own, is the other way up
 * (gpu_show turns the picture over as it draws it). A card that cannot be
 * told is given programs that turn the position over themselves
 * (gxp_hlsl.h's CLIP_AS_GL), which is the same picture but for those
 * pixels on an edge.
 *
 * Programs. Each of a draw's two is compiled and linked by itself, a
 * separable program, and a pipeline names the two in use: what comes of a
 * text then depends on the text alone, as gpu.h has it, and is kept from
 * run to run in the form the driver hands back (programs.gl in the
 * directory VITARECOMP_GPU_CACHE names), when the driver hands one back.
 *
 * Threads. A context belongs to one thread at a time, and the title's
 * threads are host threads that take turns (sched.h): the calls here come
 * from whichever of them has the baton. So the context goes with the
 * baton. The thread that holds it lets it go when it hands the baton on
 * (vita_sched_on_handover), and the next call here, from whatever thread,
 * takes it up. What that costs is two calls into the library for each
 * time a thread that has drawn gives way, which a title with one thread
 * that draws does once or twice a frame, each time it waits: a tenth to a
 * quarter of a millisecond a frame as measured, on the console's emulator
 * and on Windows. VITARECOMP_GPU_DEBUG says at the end how many times it
 * was and how long they took.
 *
 * The library is found at run time, a function at a time by its name: the
 * system's own loader has only the oldest of them, and nothing is added to
 * what a program built with this ships.
 */

#include "gpu.h"
#include "sched.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- the library's own names -------------------------------------------------------------------
 * Its types, the numbers it knows things by, and the functions used: all
 * as its registry gives them. */

typedef unsigned int  GLenum, GLuint, GLbitfield;
typedef int           GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float         GLfloat;
typedef char          GLchar;
typedef ptrdiff_t     GLintptr, GLsizeiptr;
typedef uint64_t      GLuint64;

enum {
    GL_TRIANGLES = 0x0004, GL_TRIANGLE_STRIP = 0x0005,
    GL_NEVER = 0x0200,                              /* and less, equal, less or equal, greater, not equal, greater or equal, always after it */
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR = 0x0301, GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305, GL_DST_COLOR = 0x0306,
    GL_ONE_MINUS_DST_COLOR = 0x0307, GL_SRC_ALPHA_SATURATE = 0x0308,
    GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008, GL_FUNC_SUBTRACT = 0x800A, GL_FUNC_REVERSE_SUBTRACT = 0x800B,
    GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_FRONT_AND_BACK = 0x0408, GL_CW = 0x0900, GL_CCW = 0x0901,
    GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_DITHER = 0x0BD0, GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11, GL_DEPTH_CLAMP = 0x864F, GL_CLIP_DISTANCE0 = 0x3000, GL_FRAMEBUFFER_SRGB = 0x8DB9,
    GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_ALIGNMENT = 0x0D05,
    GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_2D_ARRAY = 0x8C1A, GL_TEXTURE_BUFFER = 0x8C2A, GL_TEXTURE0 = 0x84C0,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406, GL_HALF_FLOAT = 0x140B,
    GL_UNSIGNED_INT_24_8 = 0x84FA, GL_FLOAT_32_UNSIGNED_INT_24_8_REV = 0x8DAD,
    GL_STENCIL_INDEX = 0x1901, GL_DEPTH_COMPONENT = 0x1902, GL_RED = 0x1903, GL_RGBA = 0x1908, GL_BGRA = 0x80E1,
    GL_DEPTH_STENCIL = 0x84F9, GL_RED_INTEGER = 0x8D94, GL_RG_INTEGER = 0x8228, GL_RGBA_INTEGER = 0x8D99,
    GL_RGBA8 = 0x8058, GL_SRGB8_ALPHA8 = 0x8C43, GL_RGBA16F = 0x881A, GL_RGBA32F = 0x8814, GL_R32UI = 0x8236, GL_RG32UI = 0x823C,
    GL_RGBA32UI = 0x8D70, GL_DEPTH_COMPONENT16 = 0x81A5, GL_DEPTH24_STENCIL8 = 0x88F0, GL_DEPTH32F_STENCIL8 = 0x8CAD,
    GL_KEEP = 0x1E00, GL_REPLACE = 0x1E01, GL_INCR = 0x1E02, GL_DECR = 0x1E03, GL_INVERT = 0x150A, GL_INCR_WRAP = 0x8507,
    GL_DECR_WRAP = 0x8508,
    GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_MAJOR_VERSION = 0x821B, GL_MINOR_VERSION = 0x821C, GL_NUM_EXTENSIONS = 0x821D,
    GL_EXTENSIONS = 0x1F03, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D, GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_TEXTURE_WRAP_R = 0x8072, GL_TEXTURE_MIN_LOD = 0x813A, GL_TEXTURE_MAX_LOD = 0x813B, GL_TEXTURE_LOD_BIAS = 0x8501,
    GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_MIRRORED_REPEAT = 0x8370, GL_MIRROR_CLAMP_TO_EDGE = 0x8743,
    GL_DEPTH_STENCIL_TEXTURE_MODE = 0x90EA, GL_TEXTURE_SWIZZLE_G = 0x8E43,
    GL_COLOR = 0x1800, GL_DEPTH = 0x1801, GL_STENCIL = 0x1802, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNIFORM_BUFFER = 0x8A11, GL_STREAM_DRAW = 0x88E0,
    GL_STATIC_DRAW = 0x88E4, GL_MAP_WRITE_BIT = 0x0002, GL_MAP_INVALIDATE_RANGE_BIT = 0x0004, GL_MAP_UNSYNCHRONIZED_BIT = 0x0020,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_VERTEX_SHADER_BIT = 0x0001, GL_FRAGMENT_SHADER_BIT = 0x0002, GL_PROGRAM_SEPARABLE = 0x8258,
    GL_PROGRAM_BINARY_RETRIEVABLE_HINT = 0x8257, GL_PROGRAM_BINARY_LENGTH = 0x8741, GL_NUM_PROGRAM_BINARY_FORMATS = 0x87FE,
    GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_STENCIL_ATTACHMENT = 0x8D20,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_SAMPLES_PASSED = 0x8914, GL_QUERY_RESULT = 0x8866,
    GL_UPPER_LEFT = 0x8CA2, GL_ZERO_TO_ONE = 0x935F,
    GL_DEBUG_OUTPUT = 0x92E0, GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242, GL_DEBUG_SEVERITY_NOTIFICATION = 0x826B
};

#if defined(_WIN32)
#define GLCALL __stdcall
#else
#define GLCALL
#endif

typedef void (GLCALL *gl_said)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *user);

#define GL_FUNCTIONS(F) \
    F(const GLubyte *, GetString, (GLenum)) \
    F(const GLubyte *, GetStringi, (GLenum, GLuint)) \
    F(void, GetIntegerv, (GLenum, GLint *)) \
    F(GLenum, GetError, (void)) \
    F(void, Enable, (GLenum)) \
    F(void, Disable, (GLenum)) \
    F(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    F(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    F(void, DepthFunc, (GLenum)) \
    F(void, DepthMask, (GLboolean)) \
    F(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    F(void, CullFace, (GLenum)) \
    F(void, FrontFace, (GLenum)) \
    F(void, Clear, (GLbitfield)) \
    F(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void, PixelStorei, (GLenum, GLint)) \
    F(void, Finish, (void)) \
    F(void, GenTextures, (GLsizei, GLuint *)) \
    F(void, DeleteTextures, (GLsizei, const GLuint *)) \
    F(void, BindTexture, (GLenum, GLuint)) \
    F(void, ActiveTexture, (GLenum)) \
    F(void, TexParameteri, (GLenum, GLenum, GLint)) \
    F(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    F(void, TexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)) \
    F(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    F(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    F(void, GetTexImage, (GLenum, GLint, GLenum, GLenum, void *)) \
    F(void, TextureView, (GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint)) \
    F(void, CopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei)) \
    F(void, TexBuffer, (GLenum, GLenum, GLuint)) \
    F(void, GenSamplers, (GLsizei, GLuint *)) \
    F(void, DeleteSamplers, (GLsizei, const GLuint *)) \
    F(void, BindSampler, (GLuint, GLuint)) \
    F(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    F(void, SamplerParameterf, (GLuint, GLenum, GLfloat)) \
    F(void, GenBuffers, (GLsizei, GLuint *)) \
    F(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    F(void, BindBuffer, (GLenum, GLuint)) \
    F(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    F(void *, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    F(GLboolean, UnmapBuffer, (GLenum)) \
    F(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
    F(void, GenVertexArrays, (GLsizei, GLuint *)) \
    F(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
    F(void, BindVertexArray, (GLuint)) \
    F(void, EnableVertexAttribArray, (GLuint)) \
    F(void, DisableVertexAttribArray, (GLuint)) \
    F(void, VertexAttribIFormat, (GLuint, GLint, GLenum, GLuint)) \
    F(void, VertexAttribBinding, (GLuint, GLuint)) \
    F(void, BindVertexBuffer, (GLuint, GLuint, GLintptr, GLsizei)) \
    F(void, DrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    F(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    F(void, GenFramebuffers, (GLsizei, GLuint *)) \
    F(void, BindFramebuffer, (GLenum, GLuint)) \
    F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    F(GLenum, CheckFramebufferStatus, (GLenum)) \
    F(void, DrawBuffers, (GLsizei, const GLenum *)) \
    F(void, ClearBufferfi, (GLenum, GLint, GLfloat, GLint)) \
    F(void, ClearBufferfv, (GLenum, GLint, const GLfloat *)) \
    F(void, ClearBufferiv, (GLenum, GLint, const GLint *)) \
    F(void, BlendEquationSeparate, (GLenum, GLenum)) \
    F(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    F(void, StencilFuncSeparate, (GLenum, GLenum, GLint, GLuint)) \
    F(void, StencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    F(void, StencilMaskSeparate, (GLenum, GLuint)) \
    F(void, GenQueries, (GLsizei, GLuint *)) \
    F(void, BeginQuery, (GLenum, GLuint)) \
    F(void, EndQuery, (GLenum)) \
    F(void, GetQueryObjectui64v, (GLuint, GLenum, GLuint64 *)) \
    F(GLuint, CreateShader, (GLenum)) \
    F(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    F(void, CompileShader, (GLuint)) \
    F(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(void, DeleteShader, (GLuint)) \
    F(GLuint, CreateProgram, (void)) \
    F(void, ProgramParameteri, (GLuint, GLenum, GLint)) \
    F(void, AttachShader, (GLuint, GLuint)) \
    F(void, DetachShader, (GLuint, GLuint)) \
    F(void, LinkProgram, (GLuint)) \
    F(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    F(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(void, GetProgramBinary, (GLuint, GLsizei, GLsizei *, GLenum *, void *)) \
    F(void, ProgramBinary, (GLuint, GLenum, const void *, GLsizei)) \
    F(void, DeleteProgram, (GLuint)) \
    F(void, UseProgram, (GLuint)) \
    F(void, GenProgramPipelines, (GLsizei, GLuint *)) \
    F(void, BindProgramPipeline, (GLuint)) \
    F(void, UseProgramStages, (GLuint, GLbitfield, GLuint))

static struct {
#define F(ret, name, args) ret (GLCALL *name) args;
    GL_FUNCTIONS(F)
#undef F
    /* and two that a card may be without */
    void (GLCALL *ClipControl)(GLenum, GLenum);
    void (GLCALL *DebugMessageCallback)(gl_said, const void *);
} gl;

/* --- a context, and the window ---------------------------------------------------------------
 * What differs from one system to the next, and nothing else:
 *   context_open     a context of the version wanted, current on the calling thread and drawing nowhere seen
 *   context_current  the context taken up by the calling thread, or let go by it
 *   context_proc     a function of the library by its name
 *   context_window   the host's window made what the context draws to when it draws to no frame buffer of its own
 *   context_size     that window's size
 *   context_swap     what was drawn there shown
 *   context_close    all of it given back, at the program's end */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The system has contexts for windows only. Until the host's window is
 * handed over, and for good when there is none, the context draws to a
 * window of its own that is never shown: made by a thread that does
 * nothing else, because a window goes when the thread that made it ends
 * and the title's threads end when they please. */
static struct {
    HWND   hidden, shown, refused;
    HDC    dc, shown_dc;
    HGLRC  context;
    HANDLE thread, ready;
    int    format;
    PIXELFORMATDESCRIPTOR form;
} P;

static DWORD WINAPI hidden_window(LPVOID arg) {
    WNDCLASSA wc;
    MSG m;
    (void)arg;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "vitarecomp_gpu";
    RegisterClassA(&wc);
    P.hidden = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    SetEvent(P.ready);
    if (!P.hidden) return 1;
    while (GetMessageA(&m, NULL, 0, 0) > 0) DispatchMessageA(&m);
    return 0;
}

static int context_open(int debug) {
    typedef HGLRC (WINAPI *create_context)(HDC, HGLRC, const int *);
    /* the numbers the system's extension has for: the version's two parts, the flags (of which 1 is the checking
     * kind), the profile (of which 1 is the core) */
    const int wanted[] = { 0x2091, 4, 0x2092, 3, 0x2094, debug ? 1 : 0, 0x9126, 1, 0 };
    P.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    P.thread = P.ready ? CreateThread(NULL, 0, hidden_window, NULL, 0, NULL) : NULL;
    if (!P.thread) return 0;
    WaitForSingleObject(P.ready, INFINITE);
    P.dc = P.hidden ? GetDC(P.hidden) : NULL;
    if (!P.dc) return 0;
    memset(&P.form, 0, sizeof(P.form));
    P.form.nSize = sizeof(P.form);
    P.form.nVersion = 1;
    P.form.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    P.form.iPixelType = PFD_TYPE_RGBA;
    P.form.cColorBits = 32;
    P.form.cAlphaBits = 8;
    P.format = ChoosePixelFormat(P.dc, &P.form);
    if (!P.format || !DescribePixelFormat(P.dc, P.format, sizeof(P.form), &P.form) || !SetPixelFormat(P.dc, P.format, &P.form)) return 0;
    /* a context of the oldest kind first: only with one current does the system say how one of a version is made */
    HGLRC first = wglCreateContext(P.dc);
    if (!first || !wglMakeCurrent(P.dc, first)) return 0;
    create_context create = (create_context)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (create) P.context = create(P.dc, NULL, wanted);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(first);
    return P.context && wglMakeCurrent(P.dc, P.context);
}

static int context_current(int on) {
    return on ? wglMakeCurrent(P.shown_dc ? P.shown_dc : P.dc, P.context) != 0 : wglMakeCurrent(NULL, NULL) != 0;
}

static void *context_proc(const char *name) {
    /* the newer functions are the driver's, asked of the context; the oldest are the system library's own, and
     * asking the context for one of those gets nothing, or a small number that is not an address */
    void *p = (void *)wglGetProcAddress(name);
    if ((uintptr_t)p <= 3 || (intptr_t)p == -1) {
        HMODULE lib = GetModuleHandleA("opengl32.dll");
        p = lib ? (void *)GetProcAddress(lib, name) : NULL;
    }
    return p;
}

static int context_window(void *window) {
    typedef BOOL (WINAPI *swap_interval)(int);
    HWND h = (HWND)window;
    if (P.shown == h) return 1;
    if (P.refused == h || P.shown) return 0;        /* asked once; and the context has one window, the first */
    P.refused = h;
    HDC dc = IsWindow(h) ? GetDC(h) : NULL;
    if (!dc) return 0;
    /* its pixels in the form the context was made for, which a window is given once */
    int has = GetPixelFormat(dc);
    if ((has && has != P.format) || (!has && !SetPixelFormat(dc, P.format, &P.form)) || !wglMakeCurrent(dc, P.context)) {
        wglMakeCurrent(P.dc, P.context);
        ReleaseDC(h, dc);
        return 0;
    }
    P.shown = h;
    P.shown_dc = dc;
    P.refused = NULL;
    /* shown now, and not at the screen's next turn: the host keeps its own time */
    swap_interval interval = (swap_interval)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (interval) interval(0);
    return 1;
}

static void context_size(int *width, int *height) {
    RECT rc = { 0, 0, 0, 0 };
    if (P.shown) GetClientRect(P.shown, &rc);
    *width = (int)rc.right;
    *height = (int)rc.bottom;
}

static int context_swap(void) { return SwapBuffers(P.shown_dc) != 0; }
static void context_close(void) { }                 /* the system takes it all back with the process */
static void directory(const char *path) { CreateDirectoryA(path, NULL); }

#elif defined(__SWITCH__)

#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>

/* The console's card is reached through Mesa and EGL. Before the screen is
 * handed over the context draws to no surface at all, or where the library
 * will not have that, to a surface of one pixel nobody sees; the screen is
 * taken when the first frame is shown in it and not before, because until
 * then the host may want it for a picture of its own (window_switch.c),
 * and a window is one thing or the other. */
static struct {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext context;
    EGLSurface aside, screen;
    void      *shown, *refused;
} P;

static int context_window(void *window);

static int context_open(int debug) {
    static const EGLint form[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_ALPHA_SIZE, 8, EGL_NONE };
    static const EGLint one_pixel[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    const EGLint wanted[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                              EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
                              EGL_CONTEXT_FLAGS_KHR, debug ? EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR : 0, EGL_NONE };
    EGLint found = 0;
    P.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (P.display == EGL_NO_DISPLAY || !eglInitialize(P.display, NULL, NULL)) return 0;
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(P.display, form, &P.config, 1, &found) || found < 1) return 0;
    P.context = eglCreateContext(P.display, P.config, EGL_NO_CONTEXT, wanted);
    if (P.context == EGL_NO_CONTEXT) return 0;
    P.aside = P.screen = EGL_NO_SURFACE;
    if (eglMakeCurrent(P.display, EGL_NO_SURFACE, EGL_NO_SURFACE, P.context)) return 1;
    P.aside = eglCreatePbufferSurface(P.display, P.config, one_pixel);
    if (P.aside != EGL_NO_SURFACE && eglMakeCurrent(P.display, P.aside, P.aside, P.context)) return 1;
    /* neither: the screen, then, from the start, and the host's own picture goes without */
    fprintf(stderr, "gpu: the library draws to nothing but the screen; it is taken now\n");
    return context_window(nwindowGetDefault());
}

static int context_current(int on) {
    EGLSurface to = P.screen != EGL_NO_SURFACE ? P.screen : P.aside;
    if (!on) return eglMakeCurrent(P.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_FALSE;
    /* which of the library's two is meant is a thing each thread says for itself, and a new thread means the other */
    return eglBindAPI(EGL_OPENGL_API) && eglMakeCurrent(P.display, to, to, P.context) != EGL_FALSE;
}

static void *context_proc(const char *name) { return (void *)eglGetProcAddress(name); }

static int context_window(void *window) {
    if (P.shown == window) return 1;
    if (P.refused == window || P.shown) return 0;
    P.refused = window;
    /* In its dock the console shows 1920 by 1080, and makes that of whatever smaller it is given; in the hand its
     * screen is 1280 by 720, which is what a window is unless told. Docked when the screen is taken, the window is
     * made the size that is shown as it is. */
    if (appletGetOperationMode() == AppletOperationMode_Console) nwindowSetDimensions((NWindow *)window, 1920, 1080);
    EGLSurface screen = eglCreateWindowSurface(P.display, P.config, (EGLNativeWindowType)window, NULL);
    if (screen == EGL_NO_SURFACE) return 0;
    if (!eglMakeCurrent(P.display, screen, screen, P.context)) {
        eglMakeCurrent(P.display, P.aside, P.aside, P.context);
        eglDestroySurface(P.display, screen);
        return 0;
    }
    P.screen = screen;
    P.shown = window;
    P.refused = NULL;
    return 1;
}

/* The screen's size as the console has it: the library, asked for its surface's, says nothing. */
static void context_size(int *width, int *height) {
    u32 w = 0, h = 0;
    if (P.shown && R_FAILED(nwindowGetDimensions((NWindow *)P.shown, &w, &h))) w = h = 0;
    *width = (int)w;
    *height = (int)h;
}

static int context_swap(void) { return eglSwapBuffers(P.display, P.screen) != EGL_FALSE; }

/* The screen and the card's memory given back, as the console's programs do before they end. */
static void context_close(void) {
    if (P.display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(P.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(P.display);
    P.display = EGL_NO_DISPLAY;
}

static void directory(const char *path) { mkdir(path, 0777); }

#else
#error "gpu_gl.c has a context for Windows and for the Nintendo Switch; another system wants its own few functions above"
#endif

/* --- what is kept ------------------------------------------------------------------------------ */

#define CONSTANT_BYTES 1232u            /* gxp_hlsl.h's block: 66 + 3 + 8 rows of sixteen bytes */
#define KEPT 64                         /* samplers */
#define QUERIES 4096
#define VERTEX_UNITS 18u                /* gxp_hlsl.h: a fragment program's textures are at units 0 to 17, a vertex program's after them */
#define UNITS 36u
#define MAKING UNITS                    /* and one more unit, which no program looks at: where a texture is made and filled */
#define MEMORY_UNIT 16u
#define NOW_UNIT 17u

struct gpu_shader { GLuint program; int fragment; };
struct gpu_texture { GLuint tex; GLenum kind; uint32_t width, height; };
struct gpu_mesh { GLuint array, vertices, indices; uint32_t index_count, stride; };

struct gpu_surface {
    unsigned format;
    uint32_t width, height;
    GLuint tex;                         /* what is drawn to, and read as it was drawn: a colour on the display's curve comes off it */
    GLuint raw;                         /* the same texels read as the numbers they are kept as: the texture itself where there is no curve */
    GLuint stencil;                     /* a depth's stencil, to read as a texture: in the second place of four (gpu.h) */
    GLuint copy;                        /* a colour as it stood before a draw, for a program that reads it: made when first wanted */
};

/* A buffer written from its start to its end a piece at a time, each piece
 * drawn from where it lies; at the end it is let go of, the card keeping
 * what is still to be drawn from, and begun again. */
typedef struct { GLuint id; uint32_t cap, at; } stream;

typedef struct { uint8_t key[24]; GLuint object; } kept;

static struct {
    int opened, ok, held;
    char name[200];
    char first_lines[96];               /* what every program's text begins with: the version, and how clip space is read */
    int flipped;                        /* the card reads clip space as Direct3D does; otherwise the programs turn it over */
    int mirror_once;                    /* the card has the address mode that mirrors once and then holds */
    GLint binary_formats, uniform_step;

    /* the list in hand */
    gpu_surface *colour, *depth;
    int in_list, failed;
    GLuint frame, frame_colour, frame_depth;    /* the frame buffer lists are drawn through, and what is attached to it */
    GLuint aside;                               /* and one for everything else */
    GLuint queries[QUERIES];
    unsigned query_count;

    GLuint array;                               /* how a draw's own vertices are read */
    unsigned arrays_on;
    stream vertices, indices, uniforms;
    GLuint memory[2], memory_tex[2];
    GLuint pipeline, stage[2];
    GLuint whole, nothing, show;                /* three corners that cover a target; a fragment program that writes nothing; the picture in a window */
    kept samplers[KEPT];
    unsigned sampler_count;

    /* what the library was last told, so that it is told only what changes */
    GLuint unit_tex[UNITS], unit_sampler[UNITS];
    GLenum unit_kind[UNITS];
    GLuint bound_array;
    struct {
        int valid;
        uint8_t cull, depth_func, depth_write, stencil_read, stencil_write, stencil_ref, blend, write_mask, plane;
        uint8_t blending[6];
        gpu_stencil_face stencil[2];
        int32_t scissor[4];
    } set;

    /* for gpu_show */
    gpu_surface *shown;
    int shown_once, back_width, back_height;

    /* how often the context changed hands, and what that took */
    uint64_t moves, move_ns;
    int timed;
} G;

static uint64_t now_ns(void) {
    struct timespec t;
    if (!G.timed) return 0;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* --- the context goes with the baton ------------------------------------------------------------ */

/* The running thread is about to give way (sched.h): what it holds of the card it lets go. */
static void leave(void) {
    if (!G.held) return;
    uint64_t began = now_ns();
    context_current(0);
    G.held = 0;
    G.move_ns += now_ns() - began;
}

/* The calling thread takes the context up, if it is not the one that has it. 0 when it cannot. */
static int enter(void) {
    static int told;
    if (!G.ok) return 0;
    if (G.held) return 1;
    uint64_t began = now_ns();
    if (!context_current(1)) {
        if (!told) { told = 1; fprintf(stderr, "gpu: the card's context cannot be taken up by another thread\n"); }
        return 0;
    }
    G.held = 1;
    G.moves++;
    G.move_ns += now_ns() - began;
    return 1;
}

static void closing_more(void);

static void closing(void) {
    if (G.timed && G.moves)
        fprintf(stderr, "gpu: the card's context changed hands %llu times, %.1f ms in all (%.0f us a time)\n", (unsigned long long)G.moves,
                (double)G.move_ns * 1e-6, (double)G.move_ns * 1e-3 / (double)G.moves);
    closing_more();
    context_close();
}

static void GLCALL said(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *user) {
    static unsigned count;
    (void)source; (void)type; (void)id; (void)length; (void)user;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || count >= 200) return;
    count++;
    fprintf(stderr, "gpu: the library says: %s\n", message);
}

/* --- forms ------------------------------------------------------------------------------------- */

static int is_depth(unsigned format) { return format == GPU_D16 || format == GPU_D24S8 || format == GPU_D32FS8; }
static int has_stencil(unsigned format) { return format == GPU_D24S8 || format == GPU_D32FS8; }
static int has_curve(unsigned format) { return format == GPU_RGBA8_SRGB || format == GPU_BGRA8_SRGB; }

/* How the card keeps a form. */
static GLenum kept_as(unsigned f) {
    switch (f) {
    case GPU_RGBA8: case GPU_BGRA8:           return GL_RGBA8;         /* blue first is how the bytes travel, not how they are kept */
    case GPU_RGBA8_SRGB: case GPU_BGRA8_SRGB: return GL_SRGB8_ALPHA8;
    case GPU_RGBA16F:    return GL_RGBA16F;
    case GPU_RGBA32F:    return GL_RGBA32F;
    case GPU_R32UI:      return GL_R32UI;
    case GPU_RG32UI:     return GL_RG32UI;
    case GPU_RGBA32UI:   return GL_RGBA32UI;
    case GPU_D16:        return GL_DEPTH_COMPONENT16;
    case GPU_D24S8:      return GL_DEPTH24_STENCIL8;
    case GPU_D32FS8:     return GL_DEPTH32F_STENCIL8;
    default:             return 0;
    }
}

/* And how its texels travel to and from memory: what they are, and in what pieces. */
static void travels_as(unsigned f, GLenum *what, GLenum *pieces) {
    switch (f) {
    case GPU_RGBA8: case GPU_RGBA8_SRGB: *what = GL_RGBA; *pieces = GL_UNSIGNED_BYTE; break;
    case GPU_BGRA8: case GPU_BGRA8_SRGB: *what = GL_BGRA; *pieces = GL_UNSIGNED_BYTE; break;
    case GPU_RGBA16F:  *what = GL_RGBA; *pieces = GL_HALF_FLOAT; break;
    case GPU_RGBA32F:  *what = GL_RGBA; *pieces = GL_FLOAT; break;
    case GPU_R32UI:    *what = GL_RED_INTEGER; *pieces = GL_UNSIGNED_INT; break;
    case GPU_RG32UI:   *what = GL_RG_INTEGER; *pieces = GL_UNSIGNED_INT; break;
    case GPU_RGBA32UI: *what = GL_RGBA_INTEGER; *pieces = GL_UNSIGNED_INT; break;
    case GPU_D16:      *what = GL_DEPTH_COMPONENT; *pieces = GL_UNSIGNED_SHORT; break;
    case GPU_D24S8:    *what = GL_DEPTH_STENCIL; *pieces = GL_UNSIGNED_INT_24_8; break;
    default:           *what = GL_DEPTH_STENCIL; *pieces = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; break;
    }
}

static unsigned texel_bytes(unsigned f) {
    switch (f) {
    case GPU_RGBA16F: case GPU_RG32UI: case GPU_D32FS8: return 8;
    case GPU_RGBA32F: case GPU_RGBA32UI: return 16;
    case GPU_D16: return 2;
    default: return 4;
    }
}

/* Whether the library has complained since it was last asked. */
static int complained(void) {
    int any = 0;
    for (int k = 0; k < 16 && gl.GetError() != 0; k++) any = 1;
    return any;
}

/* --- textures bound, by unit --------------------------------------------------------------------- */

static void bind(unsigned unit, GLenum kind, GLuint tex, GLuint sampler) {
    if (G.unit_tex[unit] != tex || G.unit_kind[unit] != kind) {
        gl.ActiveTexture(GL_TEXTURE0 + unit);
        /* a unit has a place for each kind of texture: what was in another kind's place is taken out of it */
        if (G.unit_tex[unit] && G.unit_kind[unit] != kind) gl.BindTexture(G.unit_kind[unit], 0);
        gl.BindTexture(kind, tex);
        G.unit_tex[unit] = tex;
        G.unit_kind[unit] = kind;
    }
    if (G.unit_sampler[unit] != sampler) {
        gl.BindSampler(unit, sampler);
        G.unit_sampler[unit] = sampler;
    }
}

/* Nothing stays bound: a surface that was read as a texture may be drawn to next. */
static void unbind_all(void) {
    for (unsigned unit = 0; unit < UNITS; unit++)
        if (G.unit_tex[unit]) bind(unit, G.unit_kind[unit], 0, 0);
}

/* A texture about to be made or filled, on the unit kept for that. */
static void making(GLenum kind, GLuint tex) {
    gl.ActiveTexture(GL_TEXTURE0 + MAKING);
    gl.BindTexture(kind, tex);
}

/* A texture that was never bound is forgotten wherever it was thought to be. */
static void forget(GLuint tex) {
    if (!tex) return;
    for (unsigned unit = 0; unit < UNITS; unit++)
        if (G.unit_tex[unit] == tex) G.unit_tex[unit] = 0;      /* the library unbinds what is deleted */
    if (G.frame_colour == tex) G.frame_colour = 0;
    if (G.frame_depth == tex) G.frame_depth = 0;
}

static void use_array(GLuint array) {
    if (G.bound_array == array) return;
    gl.BindVertexArray(array);
    G.bound_array = array;
}

static void use_programs(GLuint vertex, GLuint fragment) {
    if (G.stage[0] != vertex) { gl.UseProgramStages(G.pipeline, GL_VERTEX_SHADER_BIT, vertex); G.stage[0] = vertex; }
    if (G.stage[1] != fragment) { gl.UseProgramStages(G.pipeline, GL_FRAGMENT_SHADER_BIT, fragment); G.stage[1] = fragment; }
}

/* --- programs compiled before -------------------------------------------------------------------
 *
 * As gpu_d3d11.c keeps them: by a hash of the text, of which kind of
 * program it is and of what compiled it, in one file of records that are
 * only ever added to, each a mark, the hash, a length, a sum of the bytes,
 * the form the driver says they are in, and the bytes. What a driver hands
 * back is its own business and no other driver's, nor the same driver's
 * next version's: its name and version are part of the hash, and what it
 * will not take back is compiled again. */

#define KEPT_MARK 0x31474F5250474C56ull         /* the bytes "VLGPROG1" */

typedef struct { uint64_t mark, key[2]; uint32_t size, sum, form, spare; } kept_head;
typedef struct { uint64_t key[2]; const uint8_t *bytes; uint32_t size, form; } kept_program;

static struct {
    int asked;
    FILE *file;
    kept_program *slots;                    /* found by the hash's low bits, and the next after that */
    unsigned count, cap;
    unsigned compiled;
} K;

static void key_of(const char *text, size_t length, int fragment, uint64_t key[2]) {
    uint64_t a = 0xCBF29CE484222325ull ^ (uint64_t)fragment, b = 0x9E3779B97F4A7C15ull ^ length;
    const char *parts[3] = { G.name, G.first_lines, text };
    for (int part = 0; part < 3; part++) {
        const uint8_t *p = (const uint8_t *)parts[part];
        size_t n = part == 2 ? length : strlen(parts[part]);
        for (size_t i = 0; i < n; i++) {
            a = (a ^ p[i]) * 0x100000001B3ull;
            b = (b + p[i]) * 0x9FB21C651E98DF25ull;
            b ^= b >> 29;
        }
    }
    key[0] = a;
    key[1] = b;
}

static uint32_t sum_of(const uint8_t *p, size_t n) {
    uint32_t s = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) s = (s ^ p[i]) * 16777619u;
    return s;
}

/* Where a program of this hash is, or would go. */
static kept_program *kept_slot(const uint64_t key[2]) {
    for (unsigned at = (unsigned)key[0] & (K.cap - 1);; at = (at + 1) & (K.cap - 1)) {
        kept_program *p = &K.slots[at];
        if (!p->bytes || (p->key[0] == key[0] && p->key[1] == key[1])) return p;
    }
}

/* Remember one. The bytes must stay where they are. Returns 0 when it was not taken: it was there already, or there is no room to be had. */
static int kept_note(const uint64_t key[2], const uint8_t *bytes, uint32_t size, uint32_t form) {
    if ((K.count + 1) * 2 > K.cap) {
        unsigned cap = K.cap ? K.cap * 2 : 1024, old_cap = K.cap;
        kept_program *old = K.slots, *slots = calloc(cap, sizeof(*slots));
        if (!slots) return 0;
        K.slots = slots;
        K.cap = cap;
        for (unsigned i = 0; i < old_cap; i++)
            if (old[i].bytes) *kept_slot(old[i].key) = old[i];
        free(old);
    }
    kept_program *p = kept_slot(key);
    if (p->bytes) return 0;
    p->key[0] = key[0];
    p->key[1] = key[1];
    p->bytes = bytes;
    p->size = size;
    p->form = form;
    K.count++;
    return 1;
}

static void kept_open(void) {
    const char *dir = getenv("VITARECOMP_GPU_CACHE");
    char path[1024];
    long size;
    K.asked = 1;
    if (!dir || !*dir || G.binary_formats < 1 || snprintf(path, sizeof(path), "%s/programs.gl", dir) >= (int)sizeof(path)) return;
    directory(dir);                                 /* the last of its names, when that is all that is missing */
    K.file = fopen(path, "ab+");
    if (!K.file) {
        fprintf(stderr, "gpu: %s cannot be opened; compiled programs are not kept\n", path);
        return;
    }
    if (fseek(K.file, 0, SEEK_END) || (size = ftell(K.file)) <= 0 || size > 0x40000000 || fseek(K.file, 0, SEEK_SET)) return;
    uint8_t *all = malloc((size_t)size);            /* never let go of: the programs are read from where they lie in it */
    size_t got = all ? fread(all, 1, (size_t)size, K.file) : 0;
    for (size_t at = 0; at + sizeof(kept_head) <= got;) {
        kept_head h;
        memcpy(&h, all + at, sizeof(h));
        if (h.mark != KEPT_MARK || !h.size || h.size > got - at - sizeof(h) || sum_of(all + at + sizeof(h), h.size) != h.sum) { at++; continue; }
        kept_note(h.key, all + at + sizeof(h), h.size, h.form);
        at += sizeof(h) + (((size_t)h.size + 7u) & ~(size_t)7u);
    }
}

static void kept_add(const uint64_t key[2], GLuint program) {
    kept_head h;
    GLint size = 0;
    GLsizei got = 0;
    GLenum form = 0;
    if (!K.file) return;
    gl.GetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &size);
    if (size <= 0 || size > 0x1000000) return;
    size_t total = sizeof(h) + (((size_t)size + 7u) & ~(size_t)7u);
    uint8_t *record = calloc(1, total);
    if (!record) return;
    gl.GetProgramBinary(program, size, &got, &form, record + sizeof(h));
    if (got <= 0 || got > size || complained()) { free(record); return; }
    memset(&h, 0, sizeof(h));
    h.mark = KEPT_MARK;
    h.key[0] = key[0];
    h.key[1] = key[1];
    h.size = (uint32_t)got;
    h.sum = sum_of(record + sizeof(h), (size_t)got);
    h.form = form;
    memcpy(record, &h, sizeof(h));
    total = sizeof(h) + (((size_t)got + 7u) & ~(size_t)7u);
    /* one write, to the end wherever the end is by now */
    fwrite(record, 1, total, K.file);
    fflush(K.file);
    if (!kept_note(key, record + sizeof(h), (uint32_t)got, form)) free(record);
}

unsigned gpu_shaders_compiled(void) { return K.compiled; }

/* --- shaders --------------------------------------------------------------------------------- */

/* A program of one kind from its text, linked by itself. 0 with the compiler's complaint in `log`. */
static GLuint program_from(int fragment, const char *text, int keep, char *log, size_t log_size) {
    const GLchar *lines[2] = { G.first_lines, text };
    GLint ok = 0;
    GLuint shader = gl.CreateShader(fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER), program = 0;
    if (!shader) return 0;
    gl.ShaderSource(shader, 2, lines, NULL);
    gl.CompileShader(shader);
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (log && log_size) gl.GetShaderInfoLog(shader, (GLsizei)log_size, NULL, log);
        gl.DeleteShader(shader);
        return 0;
    }
    program = gl.CreateProgram();
    if (program) {
        gl.ProgramParameteri(program, GL_PROGRAM_SEPARABLE, 1);
        if (keep) gl.ProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, 1);
        gl.AttachShader(program, shader);
        gl.LinkProgram(program);
        gl.DetachShader(program, shader);
        gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            if (log && log_size) gl.GetProgramInfoLog(program, (GLsizei)log_size, NULL, log);
            gl.DeleteProgram(program);
            program = 0;
        }
    }
    gl.DeleteShader(shader);
    return program;
}

#if defined(__SWITCH__)

/* On the console the compiler is not run where it is asked for. It is
 * Mesa's, it is deep, and the thread that asks may be any of the title's:
 * one the console's C library made, with 128 KiB of stack, which the
 * title's own code has the use of too. So a program is compiled on a
 * thread made for the purpose with a stack that is certainly enough, the
 * context lent to it for as long as that takes and the asking thread
 * waiting. VITARECOMP_GPU_DEBUG says at the end how much of that stack the
 * deepest compilation used. */

#define COMPILE_STACK (4u << 20)

typedef struct { int fragment, keep; const char *text; char *log; size_t log_size; GLuint program; } compiling;

static uint32_t g_compile_deepest;

static void compile_here(void *arg) {
    compiling *c = arg;
    if (!context_current(1)) return;
    c->program = program_from(c->fragment, c->text, c->keep, c->log, c->log_size);
    context_current(0);
}

static GLuint program_made(int fragment, const char *text, int keep, char *log, size_t log_size) {
    static uint8_t *stack;
    compiling c = { fragment, keep, text, log, log_size, 0 };
    Thread thread;
    if (!stack) stack = aligned_alloc(0x1000, COMPILE_STACK);
    if (!stack) return program_from(fragment, text, keep, log, log_size);
    if (G.timed) memset(stack, 0xA5, COMPILE_STACK);
    if (R_FAILED(threadCreate(&thread, compile_here, &c, stack, COMPILE_STACK, 0x3B, -2))) return program_from(fragment, text, keep, log, log_size);
    context_current(0);
    if (R_SUCCEEDED(threadStart(&thread))) threadWaitForExit(&thread);
    threadClose(&thread);
    if (!context_current(1)) { G.held = 0; return 0; }
    if (G.timed) {
        /* a stack is used from its top down: what the compiler never reached still holds what it was filled with */
        uint32_t untouched = 0;
        while (untouched < COMPILE_STACK && stack[untouched] == 0xA5) untouched++;
        if (COMPILE_STACK - untouched > g_compile_deepest) g_compile_deepest = COMPILE_STACK - untouched;
    }
    return c.program;
}

static void closing_more(void) {
    if (G.timed && g_compile_deepest)
        fprintf(stderr, "gpu: the deepest compilation used %u KiB of stack\n", (g_compile_deepest + 1023u) / 1024u);
}

#else
#define program_made program_from
static void closing_more(void) { }
#endif

gpu_shader *gpu_shader_make(int fragment, const char *text, unsigned pa_words, char *log, size_t log_size) {
    uint64_t key[2];
    GLuint program = 0;
    (void)pa_words;                             /* the text says what a vertex is, and how a draw's vertices lie says the rest */
    if (log && log_size) log[0] = '\0';
    if (!enter() || !text) return NULL;
    if (!K.asked) kept_open();
    key_of(text, strlen(text), fragment, key);
    if (K.cap) {
        const kept_program *before = kept_slot(key);
        if (before->bytes && (program = gl.CreateProgram()) != 0) {
            GLint ok = 0;
            gl.ProgramBinary(program, before->form, before->bytes, (GLsizei)before->size);
            gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok || complained()) { gl.DeleteProgram(program); program = 0; }    /* another driver's, or another day's */
        }
    }
    if (!program) {
        program = program_made(fragment, text, K.file != NULL, log, log_size);
        if (!program) {
            if (log && log_size && !log[0]) snprintf(log, log_size, "the card refused the program and said nothing");
            complained();
            return NULL;
        }
        K.compiled++;
        kept_add(key, program);
    }
    gpu_shader *s = calloc(1, sizeof(*s));
    if (!s) { gl.DeleteProgram(program); return NULL; }
    s->program = program;
    s->fragment = fragment != 0;
    return s;
}

void gpu_shader_free(gpu_shader *s) {
    if (!s) return;
    if (enter()) {
        if (G.stage[0] == s->program || G.stage[1] == s->program) {
            gl.UseProgramStages(G.pipeline, GL_VERTEX_SHADER_BIT | GL_FRAGMENT_SHADER_BIT, 0);
            G.stage[0] = G.stage[1] = 0;
        }
        gl.DeleteProgram(s->program);
    }
    free(s);
}

/* --- the card ------------------------------------------------------------------------------- */

static int has_extension(const char *name) {
    GLint count = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++) {
        const char *e = (const char *)gl.GetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name)) return 1;
    }
    return 0;
}

int gpu_language(void) { return GPU_GLSL; }

int gpu_open(void) {
    /* three corners that cover whatever is drawn to, by their numbers alone */
    static const char WHOLE[] =
        "out gl_PerVertex { vec4 gl_Position; };\n"
        "void main() { uint n = uint(gl_VertexID); gl_Position = vec4(float((n << 1) & 2u) * 2.0 - 1.0, 1.0 - float(n & 2u) * 2.0, 0.0, 1.0); }\n";
    static const char NOTHING[] = "void main() { }\n";
    const char *debug = getenv("VITARECOMP_GPU_DEBUG");
    GLint major = 0, minor = 0, units = 0;
    char why[400] = "";
    if (G.opened) return G.ok ? 0 : -1;
    G.opened = 1;
    G.timed = debug && *debug && *debug != '0';
    if (!context_open(G.timed)) {
        fprintf(stderr, "gpu: no OpenGL 4.3 context is to be had here\n");
        return -1;
    }
    G.held = 1;
#define F(ret, name, args) gl.name = (ret (GLCALL *) args)context_proc("gl" #name); if (!gl.name && !why[0]) snprintf(why, sizeof(why), "it has no gl" #name);
    GL_FUNCTIONS(F)
#undef F
    if (!why[0]) {
        gl.GetIntegerv(GL_MAJOR_VERSION, &major);
        gl.GetIntegerv(GL_MINOR_VERSION, &minor);
        gl.GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
        if (major < 4 || (major == 4 && minor < 3)) snprintf(why, sizeof(why), "it is version %d.%d", major, minor);
        else if (units < (GLint)UNITS + 1) snprintf(why, sizeof(why), "it has %d places for textures where %u are wanted", units, UNITS + 1);
    }
    if (why[0]) {
        fprintf(stderr, "gpu: the OpenGL here is not one to draw with: %s\n", why);
        context_current(0);
        G.held = 0;
        return -1;
    }
    snprintf(G.name, sizeof(G.name), "OpenGL %.60s on %.100s", (const char *)gl.GetString(GL_VERSION), (const char *)gl.GetString(GL_RENDERER));
    gl.ClipControl = (void (GLCALL *)(GLenum, GLenum))context_proc("glClipControl");
    G.flipped = gl.ClipControl && ((major == 4 && minor >= 5) || major > 4 || has_extension("GL_ARB_clip_control"));
    G.mirror_once = (major == 4 && minor >= 4) || major > 4 || has_extension("GL_ARB_texture_mirror_clamp_to_edge") ||
                    has_extension("GL_EXT_texture_mirror_clamp") || has_extension("GL_ATI_texture_mirror_once");
    snprintf(G.first_lines, sizeof(G.first_lines), "#version 430 core\n%s", G.flipped ? "" : "#define CLIP_AS_GL 1\n");
    gl.GetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &G.binary_formats);
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &G.uniform_step);
    if (G.uniform_step < 1) G.uniform_step = 1;
    if (G.timed) {
        gl.DebugMessageCallback = (void (GLCALL *)(gl_said, const void *))context_proc("glDebugMessageCallback");
        if (gl.DebugMessageCallback) {
            gl.Enable(GL_DEBUG_OUTPUT);
            gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            gl.DebugMessageCallback(said, NULL);
        }
    }

    /* what stays as it is set here: bytes go to and from memory with no gaps the library puts in; a colour's bytes
     * are not scattered; a surface on the display's curve is written on it; depth nearer than the near plane is held
     * to it and drawn; and clip space is read as the programs leave it, or the winding that their turning it over
     * turns round is turned back */
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.Disable(GL_DITHER);
    gl.Enable(GL_FRAMEBUFFER_SRGB);
    gl.Enable(GL_DEPTH_CLAMP);
    if (G.flipped) gl.ClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    else gl.FrontFace(GL_CW);

    gl.GenFramebuffers(1, &G.frame);
    gl.GenFramebuffers(1, &G.aside);
    gl.GenBuffers(1, &G.vertices.id);
    gl.GenBuffers(1, &G.indices.id);
    gl.GenBuffers(1, &G.uniforms.id);
    /* a draw's own vertices: words, four to an input, sixteen inputs at the most, from one buffer */
    gl.GenVertexArrays(1, &G.array);
    use_array(G.array);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.indices.id);
    for (GLuint k = 0; k < 16; k++) {
        gl.VertexAttribIFormat(k, 4, GL_UNSIGNED_INT, k * 16);
        gl.VertexAttribBinding(k, 0);
    }
    /* the words a load instruction reaches, by stage: a buffer and the texture that is how a program sees it */
    gl.GenBuffers(2, G.memory);
    gl.GenTextures(2, G.memory_tex);
    for (int k = 0; k < 2; k++) {
        static const uint32_t NOTHING_YET = 0;
        gl.BindBuffer(GL_ARRAY_BUFFER, G.memory[k]);
        gl.BufferData(GL_ARRAY_BUFFER, 4, &NOTHING_YET, GL_STREAM_DRAW);
        making(GL_TEXTURE_BUFFER, G.memory_tex[k]);
        gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, G.memory[k]);
    }
    gl.GenProgramPipelines(1, &G.pipeline);
    gl.UseProgram(0);
    gl.BindProgramPipeline(G.pipeline);
    G.whole = program_made(0, WHOLE, 0, why, sizeof(why));
    G.nothing = G.whole ? program_made(1, NOTHING, 0, why, sizeof(why)) : 0;
    if (!G.whole || !G.nothing || complained()) {
        fprintf(stderr, "gpu: the OpenGL here is not one to draw with: its first programs did not compile (%.200s)\n", why);
        context_current(0);
        G.held = 0;
        return -1;
    }
    fprintf(stderr, "gpu: %s; clip space %s, a texture mirrored once %s, compiled programs %s\n", G.name,
            G.flipped ? "read as the programs leave it" : "turned over by the programs", G.mirror_once ? "is had" : "is not had",
            G.binary_formats > 0 ? "can be kept" : "cannot be kept");
    vita_sched_on_handover(leave);
    atexit(closing);
    G.ok = 1;
    return 0;
}

/* A card reached this way cannot hand a program its target to read and write in order. */
int gpu_ordered(unsigned format) { (void)format; return 0; }

const char *gpu_name(void) { return G.ok ? G.name : "nothing"; }

/* --- textures --------------------------------------------------------------------------------- */

gpu_texture *gpu_texture_make(unsigned format, unsigned width, unsigned height, unsigned levels, int cube,
                              const void *const *images) {
    unsigned faces = cube ? 6 : 1;
    GLenum kept_form = kept_as(format), what, pieces;
    if (!width || !height || !levels || levels > 16 || !kept_form || is_depth(format) || !enter()) return NULL;
    for (unsigned i = 0; i < levels * faces; i++)
        if (!images[i]) return NULL;
    gpu_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    travels_as(format, &what, &pieces);
    t->kind = cube ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;       /* a cube is six faces in a row, sampled as such (gxp_hlsl.c's cube_at) */
    t->width = width;
    t->height = height;
    gl.GenTextures(1, &t->tex);
    making(t->kind, t->tex);
    if (cube) gl.TexStorage3D(t->kind, (GLsizei)levels, kept_form, (GLsizei)width, (GLsizei)height, 6);
    else gl.TexStorage2D(t->kind, (GLsizei)levels, kept_form, (GLsizei)width, (GLsizei)height);
    /* the images come a level's faces together */
    for (unsigned level = 0; level < levels; level++) {
        GLsizei w = (GLsizei)(width >> level ? width >> level : 1), h = (GLsizei)(height >> level ? height >> level : 1);
        for (unsigned face = 0; face < faces; face++) {
            const void *image = images[level * faces + face];
            if (cube) gl.TexSubImage3D(t->kind, (GLint)level, 0, 0, (GLint)face, w, h, 1, what, pieces, image);
            else gl.TexSubImage2D(t->kind, (GLint)level, 0, 0, w, h, what, pieces, image);
        }
    }
    /* read by a program with no sampler of its own to say how, it is read a texel at a time */
    gl.TexParameteri(t->kind, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
    gl.TexParameteri(t->kind, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (complained()) { gpu_texture_free(t); return NULL; }
    return t;
}

gpu_texture *gpu_texture_blank(unsigned width, unsigned height) {
    if (!width || !height || !enter()) return NULL;
    gpu_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->kind = GL_TEXTURE_2D;
    t->width = width;
    t->height = height;
    gl.GenTextures(1, &t->tex);
    making(GL_TEXTURE_2D, t->tex);
    gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, (GLsizei)width, (GLsizei)height);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (complained()) { gpu_texture_free(t); return NULL; }
    return t;
}

void gpu_texture_free(gpu_texture *t) {
    if (!t) return;
    if (t->tex && enter()) {
        forget(t->tex);
        gl.DeleteTextures(1, &t->tex);
    }
    free(t);
}

/* --- vertices and indices that stay ------------------------------------------------------------- */

gpu_mesh *gpu_mesh_make(const void *vertices, uint32_t vertex_count, uint32_t stride, const uint32_t *indices, uint32_t index_count) {
    uint32_t bytes = vertex_count * stride;
    if (!indices || !index_count || !enter()) return NULL;
    gpu_mesh *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    gl.GenBuffers(1, &m->vertices);
    gl.GenBuffers(1, &m->indices);
    gl.GenVertexArrays(1, &m->array);
    if (bytes) {
        gl.BindBuffer(GL_ARRAY_BUFFER, m->vertices);
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, vertices, GL_STATIC_DRAW);
    }
    /* which buffer the indices are in is part of how the vertices are read, and so set with that in hand */
    use_array(m->array);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->indices);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)index_count * 4, indices, GL_STATIC_DRAW);
    if (bytes) {
        gl.BindVertexBuffer(0, m->vertices, 0, (GLsizei)stride);
        for (GLuint k = 0; k < stride / 16 && k < 16; k++) {
            gl.VertexAttribIFormat(k, 4, GL_UNSIGNED_INT, k * 16);
            gl.VertexAttribBinding(k, 0);
            gl.EnableVertexAttribArray(k);
        }
    }
    m->index_count = index_count;
    m->stride = stride;
    if (complained()) { gpu_mesh_free(m); return NULL; }
    return m;
}

void gpu_mesh_free(gpu_mesh *m) {
    if (!m) return;
    if (enter()) {
        if (G.bound_array == m->array) use_array(G.array);
        gl.DeleteVertexArrays(1, &m->array);
        gl.DeleteBuffers(1, &m->vertices);
        gl.DeleteBuffers(1, &m->indices);
    }
    free(m);
}

/* --- buffers written a piece at a time ---------------------------------------------------------- */

/* `bytes` of `data` at the next place in the buffer that is a multiple of
 * `step`, made up to `whole` bytes with nothing when that is more; where
 * it went is `at`. `more` is how much the same draw will add after this:
 * a draw's pieces are all in the buffer as it stands at the draw, and when
 * the buffer is begun again what an earlier piece was bound to is gone, so
 * it is begun again before the first of them if it would not hold them
 * all. 0 when the card has no room. */
static int add(stream *s, const void *data, uint32_t bytes, uint32_t whole, uint32_t step, uint32_t more, uint32_t *at) {
    uint32_t size = whole > bytes ? whole : bytes, from = (s->at + step - 1) / step * step;
    if (!size) { *at = from; return 1; }
    if (!s->cap || from + size + more > s->cap) {
        /* full, or never begun: the card keeps what was written for the draws that have it still to read, and this is
         * a buffer of nothing again, larger if these pieces would not fit one the size it was */
        uint32_t cap = s->cap ? s->cap : 1u << 20;
        while (cap < size + more) cap *= 2;
        gl.BindBuffer(GL_ARRAY_BUFFER, s->id);
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cap, NULL, GL_STREAM_DRAW);
        s->cap = cap;
        from = 0;
    } else {
        gl.BindBuffer(GL_ARRAY_BUFFER, s->id);
    }
    /* a part nothing has been drawn from: the card need wait for no draw before it is written */
    uint8_t *to = gl.MapBufferRange(GL_ARRAY_BUFFER, (GLintptr)from, (GLsizeiptr)size,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (!to) return 0;
    if (bytes && data) memcpy(to, data, bytes);
    else if (bytes) memset(to, 0, bytes);
    if (size > bytes) memset(to + bytes, 0, size - bytes);
    gl.UnmapBuffer(GL_ARRAY_BUFFER);
    s->at = from + size;
    *at = from;
    return 1;
}

/* --- samplers, made once of each kind met -------------------------------------------------------- */

static GLuint sampler_for(const gpu_sampler *g) {
    static const GLenum ADDRESS[4] = { GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRROR_CLAMP_TO_EDGE };
    uint8_t key[24] = { 0 };
    GLuint s = 0;
    key[0] = g->min_linear; key[1] = g->mag_linear; key[2] = g->mip; key[3] = g->address_u; key[4] = g->address_v;
    memcpy(key + 8, &g->lod_bias, 4);
    memcpy(key + 12, &g->lod_min, 4);
    memcpy(key + 16, &g->lod_max, 4);
    for (unsigned i = 0; i < G.sampler_count; i++)
        if (!memcmp(G.samplers[i].key, key, 24)) return G.samplers[i].object;
    /* a mode the card has no equal for is not drawn as another */
    if (!G.mirror_once && ((g->address_u & 3) == 3 || (g->address_v & 3) == 3)) return 0;
    gl.GenSamplers(1, &s);
    if (!s) return 0;
    /* between levels only when asked; and with one level wanted the levels are held to that one */
    gl.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, g->mip == 2 ? (g->min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                                                               : (g->min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST));
    gl.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, g->mag_linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)ADDRESS[g->address_u & 3]);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)ADDRESS[g->address_v & 3]);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl.SamplerParameterf(s, GL_TEXTURE_LOD_BIAS, g->lod_bias);
    gl.SamplerParameterf(s, GL_TEXTURE_MIN_LOD, g->lod_min);
    gl.SamplerParameterf(s, GL_TEXTURE_MAX_LOD, g->mip ? g->lod_max : g->lod_min);
    if (G.sampler_count == KEPT) {                  /* full: the oldest goes */
        for (unsigned unit = 0; unit < UNITS; unit++)
            if (G.unit_sampler[unit] == G.samplers[0].object) { gl.BindSampler(unit, 0); G.unit_sampler[unit] = 0; }
        gl.DeleteSamplers(1, &G.samplers[0].object);
        memmove(G.samplers, G.samplers + 1, (KEPT - 1) * sizeof(G.samplers[0]));
        G.sampler_count--;
    }
    memcpy(G.samplers[G.sampler_count].key, key, 24);
    G.samplers[G.sampler_count++].object = s;
    return s;
}

/* --- surfaces ----------------------------------------------------------------------------------- */

/* A texture of a surface's form and size holding nothing yet, read a texel at a time. */
static GLuint plain(GLenum kept_form, uint32_t width, uint32_t height) {
    GLuint tex = 0;
    gl.GenTextures(1, &tex);
    making(GL_TEXTURE_2D, tex);
    gl.TexStorage2D(GL_TEXTURE_2D, 1, kept_form, (GLsizei)width, (GLsizei)height);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

/* The same texels as another texture has, read in another way. */
static GLuint view_of(GLuint tex, GLenum as) {
    GLuint view = 0;
    gl.GenTextures(1, &view);
    gl.TextureView(view, GL_TEXTURE_2D, tex, as, 0, 1, 0, 1);
    making(GL_TEXTURE_2D, view);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return view;
}

gpu_surface *gpu_surface_make(unsigned format, uint32_t width, uint32_t height) {
    GLenum kept_form = kept_as(format);
    if (!width || !height || !kept_form || !enter()) return NULL;
    gpu_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->format = format;
    s->width = width;
    s->height = height;
    s->tex = plain(kept_form, width, height);
    s->raw = s->tex;
    /* A surface is read in two ways where it is read in two: bytes of colour are drawn on the display's curve and
     * read as the bytes they are; depth is drawn as depth and its stencil read as a number. Each way is a texture of
     * its own over the same texels. */
    if (has_curve(format)) s->raw = view_of(s->tex, GL_RGBA8);
    if (has_stencil(format)) {
        s->stencil = view_of(s->tex, kept_form);
        gl.TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    }
    if (complained()) { gpu_surface_free(s); return NULL; }
    return s;
}

void gpu_surface_free(gpu_surface *s) {
    if (!s) return;
    if (G.colour == s) G.colour = NULL;
    if (G.depth == s) G.depth = NULL;
    if (G.shown == s) G.shown_once = 0;             /* another may be made where it was */
    if (enter()) {
        GLuint all[4] = { s->copy, s->stencil, s->raw != s->tex ? s->raw : 0, s->tex };
        for (int k = 0; k < 4; k++) {
            if (!all[k]) continue;
            forget(all[k]);
            gl.DeleteTextures(1, &all[k]);
        }
    }
    free(s);
}

/* Twenty-four bits of depth with stencil above them, as gpu.h has a word
 * of them, and with stencil below, as the library has it. */
static void turn_words(uint32_t *words, size_t count, int to_library) {
    if (to_library) for (size_t i = 0; i < count; i++) words[i] = (words[i] << 8) | (words[i] >> 24);
    else            for (size_t i = 0; i < count; i++) words[i] = (words[i] >> 8) | (words[i] << 24);
}

static uint8_t *g_rows;
static size_t g_rows_cap;

static uint8_t *rows_of(size_t bytes) {
    if (bytes > g_rows_cap) {
        uint8_t *grown = realloc(g_rows, bytes);
        if (!grown) return NULL;
        g_rows = grown;
        g_rows_cap = bytes;
    }
    return g_rows;
}

int gpu_surface_put(gpu_surface *s, const void *rows, uint32_t pitch) {
    GLenum what, pieces;
    if (!s || !rows || G.in_list || !enter()) return -1;
    uint32_t texel = texel_bytes(s->format), row_bytes = s->width * texel;
    travels_as(s->format, &what, &pieces);
    /* rows with gaps the library cannot be told of, and depth whose words lie the other way: by way of rows that are as it wants them */
    if (s->format == GPU_D24S8 || pitch % texel) {
        uint8_t *tight = rows_of((size_t)row_bytes * s->height);
        if (!tight) return -1;
        for (uint32_t y = 0; y < s->height; y++) memcpy(tight + (size_t)y * row_bytes, (const uint8_t *)rows + (size_t)y * pitch, row_bytes);
        if (s->format == GPU_D24S8) turn_words((uint32_t *)(void *)tight, (size_t)s->width * s->height, 1);
        rows = tight;
        pitch = row_bytes;
    }
    making(GL_TEXTURE_2D, s->tex);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / texel));
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)s->width, (GLsizei)s->height, what, pieces, rows);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return complained() ? -1 : 0;
}

int gpu_surface_get(gpu_surface *s, void *rows, uint32_t pitch) {
    GLenum what, pieces;
    if (!s || !rows || G.in_list || !enter()) return -1;
    uint32_t texel = texel_bytes(s->format), row_bytes = s->width * texel;
    int loose = pitch % texel != 0;
    uint8_t *into = rows;
    travels_as(s->format, &what, &pieces);
    if (loose && !(into = rows_of((size_t)row_bytes * s->height))) return -1;
    making(GL_TEXTURE_2D, s->tex);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, loose ? 0 : (GLint)(pitch / texel));
    gl.GetTexImage(GL_TEXTURE_2D, 0, what, pieces, into);           /* which waits for the card to have drawn */
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (complained()) return -1;
    for (uint32_t y = 0; y < s->height; y++) {
        uint8_t *row = (uint8_t *)rows + (size_t)y * pitch;
        if (loose) memcpy(row, into + (size_t)y * row_bytes, row_bytes);
        if (s->format == GPU_D24S8) turn_words((uint32_t *)(void *)row, s->width, 0);
    }
    return 0;
}

/* A depth surface as what the frame buffer in hand tests against and
 * writes: its depth and its stencil in one piece, or its depth with no
 * stencil beside it; or neither. Never the depth of one and the stencil of
 * another, even for the moment between two calls. */
static void attach_depth(const gpu_surface *depth) {
    if (depth && !has_stencil(depth->format)) {
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth->tex, 0);
    } else {
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth ? depth->tex : 0, 0);
    }
}

/* The frame buffer for everything that is not a list, drawing to a colour texture, a depth surface, both or neither. */
static void aside(GLuint colour, const gpu_surface *depth) {
    static const GLenum FIRST = GL_COLOR_ATTACHMENT0, NONE = 0;
    gl.BindFramebuffer(GL_FRAMEBUFFER, G.aside);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    attach_depth(depth);
    gl.DrawBuffers(1, colour ? &FIRST : &NONE);
}

/* Back to the list's own, when one is in hand. */
static void aside_done(void) {
    gl.BindFramebuffer(GL_FRAMEBUFFER, G.in_list ? G.frame : 0);
    G.set.valid = 0;
}

void gpu_surface_clear(gpu_surface *s, float depth, uint8_t stencil) {
    if (!s || !is_depth(s->format) || !enter()) return;
    aside(0, s);
    gl.Disable(GL_SCISSOR_TEST);
    gl.DepthMask(1);
    gl.StencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
    if (has_stencil(s->format)) gl.ClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencil);
    else gl.ClearBufferfv(GL_DEPTH, 0, &depth);
    aside_done();
}

int gpu_surface_copy(gpu_surface *into, gpu_surface *from) {
    if (!into || !from || G.in_list || into->format != from->format || into->width != from->width || into->height != from->height || !enter()) return -1;
    gl.CopyImageSubData(from->tex, GL_TEXTURE_2D, 0, 0, 0, 0, into->tex, GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)from->width, (GLsizei)from->height, 1);
    return complained() ? -1 : 0;
}

int gpu_surface_copy_corner(gpu_surface *into, gpu_surface *from, uint32_t width, uint32_t height) {
    if (!into || !from || G.in_list || into->format != from->format || is_depth(into->format) || width > into->width || height > into->height ||
        width > from->width || height > from->height || !enter()) return -1;
    gl.CopyImageSubData(from->tex, GL_TEXTURE_2D, 0, 0, 0, 0, into->tex, GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)width, (GLsizei)height, 1);
    return complained() ? -1 : 0;
}

/* Here a corner of depth and stencil together can be handed from one
 * surface to another, which Direct3D cannot do. What gpu.h promises is the
 * depths as they are and one stencil throughout, and that is what is left:
 * the corner copied, and then the stencil made the one it was asked to be. */
int gpu_surface_depth_corner(gpu_surface *into, gpu_surface *from, uint8_t stencil) {
    GLint one = stencil;
    if (G.in_list || !into || !from || !is_depth(into->format) || into->format != from->format || into->width > from->width ||
        into->height > from->height || !enter()) return -1;
    gl.CopyImageSubData(from->tex, GL_TEXTURE_2D, 0, 0, 0, 0, into->tex, GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)into->width, (GLsizei)into->height, 1);
    if (has_stencil(into->format)) {
        aside(0, into);
        gl.Disable(GL_SCISSOR_TEST);
        gl.StencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        gl.ClearBufferiv(GL_STENCIL, 0, &one);
        aside_done();
    }
    return complained() ? -1 : 0;
}

/* --- a texture from a surface, and the picture in a window ------------------------------------- */

/* Three corners over the whole of what is drawn to, by `program`, with no test and nothing kept out. */
static void cover(GLuint program) {
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_CLIP_DISTANCE0);
    gl.ColorMask(1, 1, 1, 1);
    use_programs(G.whole, program);
    use_array(G.array);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    G.set.valid = 0;
}

/* A program's constants, at the block it looks for them in: 0 a vertex
 * program's, 1 a fragment program's. `another` says the same draw has a
 * second block to come. */
static int constants_at(unsigned block, const void *bytes, uint32_t count, int another) {
    uint32_t at = 0, step = (uint32_t)G.uniform_step;
    if (!add(&G.uniforms, bytes, count < CONSTANT_BYTES ? count : CONSTANT_BYTES, CONSTANT_BYTES, step, another ? CONSTANT_BYTES + step : 0, &at)) return 0;
    gl.BindBufferRange(GL_UNIFORM_BUFFER, block, G.uniforms.id, (GLintptr)at, CONSTANT_BYTES);
    return 1;
}

int gpu_convert(gpu_texture *into, gpu_surface *from, gpu_shader *program, const void *constants, uint32_t constants_bytes) {
    if (G.in_list || !into || into->kind != GL_TEXTURE_2D || !from || !program || !program->fragment || !enter()) return -1;
    if (!constants_at(1, constants, constants && constants_bytes < 64 ? constants_bytes : constants ? 64 : 0, 0)) return -1;
    aside(into->tex, NULL);
    gl.Viewport(0, 0, (GLsizei)into->width, (GLsizei)into->height);
    gl.Disable(GL_SCISSOR_TEST);
    bind(0, GL_TEXTURE_2D, from->raw, 0);
    bind(1, GL_TEXTURE_2D, from->stencil, 0);
    cover(program->program);
    /* neither stays where it was: the texture is about to be sampled and the surface drawn to */
    bind(0, GL_TEXTURE_2D, 0, 0);
    bind(1, GL_TEXTURE_2D, 0, 0);
    aside(0, NULL);
    aside_done();
    return complained() ? -1 : 0;
}

int gpu_show(void *window, gpu_surface *s, int fresh) {
    /* gpu_d3d11.c's, in this language: where in the surface a pixel of the window looks. A pixel of the window that
     * lies wholly within one of the surface's is that one; one that lies across two is as much of each as it covers;
     * and where the window is the smaller, a pixel of it is the mean of what it covers, up to four looks each way.
     * The window's rows are the library's, counted from the bottom, and the surface's are counted from the top. */
    static const char SHOW[] =
        "layout(std140, binding = 1) uniform C { vec4 RECT; vec4 SIZE; vec4 WINDOW; };\n"
        "layout(binding = 0) uniform sampler2D SRC;\n"
        "layout(location = 0) out vec4 result;\n"
        "void main() {\n"
        "    vec2 pos = vec2(gl_FragCoord.x, WINDOW.y - gl_FragCoord.y);\n"
        "    vec2 at = (pos - RECT.xy) * SIZE.zw, cell = floor(at), part = at - cell;\n"
        "    vec2 grown = max(1.0 / SIZE.zw, 1.0), shrunk = max(SIZE.zw, 1.0);\n"
        "    vec2 from = cell + clamp(part * grown, 0.0, 0.5) + clamp((part - 1.0) * grown + 0.5, 0.0, 0.5);\n"
        "    ivec2 looks = ivec2(min(ceil(shrunk), 4.0));\n"
        "    vec3 sum = vec3(0.0, 0.0, 0.0);\n"
        "    for (int j = 0; j < looks.y; j++)\n"
        "        for (int k = 0; k < looks.x; k++) {\n"
        "            vec2 aside = ((vec2(k, j) + 0.5) / vec2(looks) - 0.5) * shrunk;\n"
        "            sum += textureLod(SRC, (from + aside) / SIZE.xy, 0.0).rgb;\n"
        "        }\n"
        "    result = vec4(sum / float(looks.x * looks.y), 1.0);\n"
        "}\n";
    static const gpu_sampler SMOOTH = { 1, 1, 0, 2, 2, 0.0f, 0.0f, 0.0f };
    int width = 0, height = 0;
    if (G.in_list || !window || !enter()) return -1;
    if (s && s->format != GPU_RGBA8 && s->format != GPU_RGBA8_SRGB) return -1;
    if (!context_window(window)) return -1;
    if (!G.show) {
        char log[400];
        G.show = program_made(1, SHOW, 0, log, sizeof(log));
        if (!G.show) { complained(); return -1; }
    }
    /* put away, or of no size: there is nothing of it to see */
    context_size(&width, &height);
    if (width <= 0 || height <= 0) return 0;
    if (!fresh && G.shown_once && G.shown == s && width == G.back_width && height == G.back_height) return 0;

    /* the window is not a surface: what is written to it is the bytes the surface holds, as they are */
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Disable(GL_FRAMEBUFFER_SRGB);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(1, 1, 1, 1);
    gl.Viewport(0, 0, width, height);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    GLuint smooth = s ? sampler_for(&SMOOTH) : 0;
    if (s && smooth) {
        /* as large as fits at the surface's shape, in the middle */
        long long sw = s->width, sh = s->height;
        long w = width, t = height;
        if (w * sh > t * sw) w = (long)(t * sw / sh);
        else                 t = (long)(w * sh / sw);
        if (w < 1) w = 1;
        if (t < 1) t = 1;
        long left = (width - w) / 2, top = (height - t) / 2;
        float place[12] = { (float)left, (float)top, (float)w, (float)t,
                            (float)s->width, (float)s->height, (float)s->width / (float)w, (float)s->height / (float)t,
                            (float)width, (float)height, 0.0f, 0.0f };
        if (constants_at(1, place, sizeof(place), 0)) {
            gl.Enable(GL_SCISSOR_TEST);
            gl.Scissor((GLint)left, (GLint)(height - top - t), (GLsizei)w, (GLsizei)t);
            bind(0, GL_TEXTURE_2D, s->raw, smooth);
            cover(G.show);
            bind(0, GL_TEXTURE_2D, 0, 0);
        }
    }
    gl.Enable(GL_FRAMEBUFFER_SRGB);
    G.set.valid = 0;
    if (complained() || !context_swap()) return -1;
    G.shown = s;
    G.shown_once = 1;
    G.back_width = width;
    G.back_height = height;
    return 0;
}

/* --- a list ------------------------------------------------------------------------------------- */

int gpu_begin(gpu_surface *colour, gpu_surface *depth) {
    static const GLenum FIRST = GL_COLOR_ATTACHMENT0, NONE = 0;
    if (G.in_list || !depth || !is_depth(depth->format) || !enter()) return -1;
    if (colour && (is_depth(colour->format) || colour->width != depth->width || colour->height != depth->height)) return -1;
    GLuint first = colour ? colour->tex : 0;
    gl.BindFramebuffer(GL_FRAMEBUFFER, G.frame);
    if (G.frame_colour != first || G.frame_depth != depth->tex) {
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, first, 0);
        attach_depth(depth);
        gl.DrawBuffers(1, colour ? &FIRST : &NONE);
        G.frame_colour = G.frame_depth = 0;
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || complained()) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            return -1;
        }
        G.frame_colour = first;
        G.frame_depth = depth->tex;
    }
    gl.Viewport(0, 0, (GLsizei)depth->width, (GLsizei)depth->height);
    G.colour = colour;
    G.depth = depth;
    G.in_list = 1;
    G.failed = 0;
    G.query_count = 0;
    G.set.valid = 0;
    return 0;
}

static GLenum factor(unsigned f, int alpha) {
    switch (f) {
    case 0:  return GL_ZERO;
    case 1:  return GL_ONE;
    case 2:  return alpha ? GL_SRC_ALPHA : GL_SRC_COLOR;
    case 3:  return alpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
    case 4:  return GL_SRC_ALPHA;
    case 5:  return GL_ONE_MINUS_SRC_ALPHA;
    case 6:  return alpha ? GL_DST_ALPHA : GL_DST_COLOR;
    case 7:  return alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
    case 8:  return GL_DST_ALPHA;
    case 9:  return GL_ONE_MINUS_DST_ALPHA;
    default: return alpha ? GL_ONE : GL_SRC_ALPHA_SATURATE;
    }
}

static GLenum operation(unsigned op) {
    /* the library's "subtract" takes the destination from the source */
    return op == 2 ? GL_FUNC_SUBTRACT : op == 3 ? GL_FUNC_REVERSE_SUBTRACT : op == 4 ? GL_MIN : op == 5 ? GL_MAX : GL_FUNC_ADD;
}

static GLenum stencil_op(unsigned op) {
    static const GLenum OPS[8] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return OPS[op & 7];
}

/* The tests and the blend of a draw, said to the library where they are not what it was last told. */
static void state(const gpu_draw_call *c) {
    int all = !G.set.valid;
    uint8_t blending[6] = { c->colour_op, c->alpha_op, c->colour_src, c->colour_dst, c->alpha_src, c->alpha_dst };
    if (all) {
        gl.Enable(GL_SCISSOR_TEST);
        gl.Enable(GL_DEPTH_TEST);
        gl.Enable(GL_STENCIL_TEST);
        gl.Enable(GL_CLIP_DISTANCE0);
    }
    if (all || G.set.cull != c->cull) {
        /* counter-clockwise on the screen is the front */
        if (c->cull) { gl.Enable(GL_CULL_FACE); gl.CullFace(c->cull == 1 ? GL_BACK : GL_FRONT); }
        else gl.Disable(GL_CULL_FACE);
        G.set.cull = c->cull;
    }
    if (all || memcmp(G.set.scissor, c->scissor, sizeof(c->scissor))) {
        GLsizei w = c->scissor[2] > c->scissor[0] ? c->scissor[2] - c->scissor[0] : 0, h = c->scissor[3] > c->scissor[1] ? c->scissor[3] - c->scissor[1] : 0;
        gl.Scissor(c->scissor[0], c->scissor[1], w, h);         /* rows from the top, as the surface's are */
        memcpy(G.set.scissor, c->scissor, sizeof(c->scissor));
    }
    if (all || G.set.depth_func != c->depth_func) { gl.DepthFunc(GL_NEVER + (c->depth_func & 7)); G.set.depth_func = c->depth_func; }
    if (all || G.set.depth_write != c->depth_write) { gl.DepthMask(c->depth_write ? 1 : 0); G.set.depth_write = c->depth_write; }
    if (all || G.set.stencil_write != c->stencil_write) { gl.StencilMaskSeparate(GL_FRONT_AND_BACK, c->stencil_write); G.set.stencil_write = c->stencil_write; }
    if (all || G.set.stencil_read != c->stencil_read || G.set.stencil_ref != c->stencil_ref || memcmp(G.set.stencil, c->stencil, sizeof(c->stencil))) {
        for (int k = 0; k < 2; k++) {
            GLenum face = k ? GL_BACK : GL_FRONT;
            gl.StencilFuncSeparate(face, GL_NEVER + (c->stencil[k].func & 7), c->stencil_ref, c->stencil_read);
            gl.StencilOpSeparate(face, stencil_op(c->stencil[k].fail), stencil_op(c->stencil[k].depth_fail), stencil_op(c->stencil[k].pass));
        }
        G.set.stencil_read = c->stencil_read;
        G.set.stencil_ref = c->stencil_ref;
        memcpy(G.set.stencil, c->stencil, sizeof(c->stencil));
    }
    if (all || G.set.blend != c->blend || memcmp(G.set.blending, blending, sizeof(blending))) {
        if (c->blend) {
            gl.Enable(GL_BLEND);
            gl.BlendEquationSeparate(operation(c->colour_op), operation(c->alpha_op));
            /* an operation of 0 on one of the two is the source as it is */
            gl.BlendFuncSeparate(c->colour_op ? factor(c->colour_src, 0) : GL_ONE, c->colour_op ? factor(c->colour_dst, 0) : GL_ZERO,
                                 c->alpha_op ? factor(c->alpha_src, 1) : GL_ONE, c->alpha_op ? factor(c->alpha_dst, 1) : GL_ZERO);
        } else {
            gl.Disable(GL_BLEND);
        }
        G.set.blend = c->blend;
        memcpy(G.set.blending, blending, sizeof(blending));
    }
    if (all || G.set.write_mask != c->write_mask) {
        gl.ColorMask(c->write_mask & 1, (c->write_mask >> 1) & 1, (c->write_mask >> 2) & 1, (c->write_mask >> 3) & 1);
        G.set.write_mask = c->write_mask;
    }
    G.set.valid = 1;
}

int gpu_draw(const gpu_draw_call *c) {
    int counted = -1;
    uint32_t vertices_at = 0, indices_at = 0;
    if (!G.in_list || !c->vertex || c->vertex->fragment || (!c->mesh && (!c->index_count || !c->vertex_count)) || !enter()) return -1;
    /* the target to read and write in order was never promised (gpu_ordered) */
    if (c->reads_pixel == 2 || (c->fragment && !c->fragment->fragment)) { G.failed = 1; return -1; }

    if (!c->mesh) {
        uint32_t vertex_bytes = c->vertex_count * c->vertex_stride, index_bytes = c->index_count * c->index_size;
        unsigned inputs = c->vertex_stride / 16 < 16 ? c->vertex_stride / 16 : 16;
        if (!add(&G.vertices, c->vertices, vertex_bytes, 0, 4, 0, &vertices_at) || !add(&G.indices, c->indices, index_bytes, 0, 4, 0, &indices_at)) {
            G.failed = 1;
            return -1;
        }
        use_array(G.array);
        gl.BindVertexBuffer(0, G.vertices.id, (GLintptr)vertices_at, (GLsizei)c->vertex_stride);
        for (; G.arrays_on < inputs; G.arrays_on++) gl.EnableVertexAttribArray(G.arrays_on);
        for (; G.arrays_on > inputs; G.arrays_on--) gl.DisableVertexAttribArray(G.arrays_on - 1);
    }

    for (unsigned k = 0; k < 2; k++) {
        unsigned first = k ? 0 : VERTEX_UNITS;
        if (k && !c->fragment) break;
        if (!constants_at(k, c->constants[k], c->constants[k] ? c->constants_bytes[k] : 0, !k && c->fragment)) { G.failed = 1; return -1; }
        if (c->memory[k] && c->memory_words[k]) {
            gl.BindBuffer(GL_ARRAY_BUFFER, G.memory[k]);
            gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)c->memory_words[k] * 4, c->memory[k], GL_STREAM_DRAW);
            bind(first + MEMORY_UNIT, GL_TEXTURE_BUFFER, G.memory_tex[k], 0);
        }
        for (unsigned unit = 0; unit < 16; unit++) {
            const gpu_texture *t = c->textures[k][unit];
            if (!t) continue;
            GLuint sampler = sampler_for(&c->samplers[k][unit]);
            if (!sampler) { G.failed = 1; return -1; }
            bind(first + unit, t->kind, t->tex, sampler);
        }
    }

    /* a program that reads the pixel it writes is given the target as it stood before the draw: a copy, of as much
     * of it as the draw can reach */
    gpu_surface *colour = G.colour;
    if (c->reads_pixel && colour && c->fragment) {
        int32_t x0 = c->scissor[0] > 0 ? c->scissor[0] : 0, y0 = c->scissor[1] > 0 ? c->scissor[1] : 0;
        int32_t x1 = c->scissor[2] < (int32_t)colour->width ? c->scissor[2] : (int32_t)colour->width;
        int32_t y1 = c->scissor[3] < (int32_t)colour->height ? c->scissor[3] : (int32_t)colour->height;
        if (!colour->copy) {
            colour->copy = plain(kept_as(colour->format), colour->width, colour->height);
            if (complained()) { gl.DeleteTextures(1, &colour->copy); colour->copy = 0; G.failed = 1; return -1; }
        }
        if (x1 > x0 && y1 > y0)
            gl.CopyImageSubData(colour->tex, GL_TEXTURE_2D, 0, x0, y0, 0, colour->copy, GL_TEXTURE_2D, 0, x0, y0, 0, x1 - x0, y1 - y0, 1);
        bind(NOW_UNIT, GL_TEXTURE_2D, colour->copy, 0);
    }

    state(c);
    use_programs(c->vertex->program, c->fragment ? c->fragment->program : G.nothing);
    if (c->mesh) use_array(c->mesh->array);

    if (c->count && G.query_count < QUERIES) {
        if (!G.queries[G.query_count]) gl.GenQueries(1, &G.queries[G.query_count]);
        if (G.queries[G.query_count]) {
            counted = (int)G.query_count++;
            gl.BeginQuery(GL_SAMPLES_PASSED, G.queries[counted]);
        }
    }
    if (c->mesh) gl.DrawElements(c->strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES, (GLsizei)c->mesh->index_count, GL_UNSIGNED_INT, NULL);
    else gl.DrawElements(c->strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES, (GLsizei)c->index_count, c->index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                         (const void *)(uintptr_t)indices_at);
    if (counted >= 0) gl.EndQuery(GL_SAMPLES_PASSED);
    return counted;
}

int gpu_end(uint64_t *counts, unsigned max_counts) {
    if (!G.in_list) return -1;
    G.in_list = 0;
    if (!enter()) return -1;
    /* nothing of the list stays bound: its surfaces may be read as textures by the next */
    unbind_all();
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    for (unsigned i = 0; i < G.query_count; i++) {
        GLuint64 passed = 0;
        gl.GetQueryObjectui64v(G.queries[i], GL_QUERY_RESULT, &passed);     /* which waits for the card */
        if (counts && i < max_counts) counts[i] = passed;
    }
    G.query_count = 0;
    /* a draw the library would not take is a draw that was not drawn */
    if (complained()) G.failed = 1;
    return G.failed ? -1 : 0;
}
