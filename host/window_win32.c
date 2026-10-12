/* window_win32.c — the host's window on Windows: GDI and nothing else.
 *
 * One thread owns the window and its message loop. Presenting a frame copies
 * it, swapped into the byte order a DIB wants, into a buffer the paint
 * handler stretches over the client area. A graphics card that holds the
 * frame draws it in the window for itself instead (vita_window_native), and
 * the window then paints nothing. The keyboard is tracked from the
 * window's own key messages, so it reads nothing while another window has
 * the focus. The left mouse button is a finger on the front panel: where it
 * is held on the picture is where the panel is touched. The picture keeps
 * the frame's shape in a window of any other, centred, and F11 or Alt and
 * Enter put the window over the whole screen and back. The window's icon
 * is the program's resource 101, when it has one: a game's program does.
 *
 * The window's pixels are the screen's. On a screen the system enlarges
 * things for, a program that does not say it knows is given a smaller
 * window than it is shown in and its finished picture is stretched to
 * fit, which blurs it; so the program says it knows, and a size asked for
 * is a size in the screen's own pixels.
 *
 * The keys are where a player of this console's games on a keyboard looks
 * for them: the arrows are the directional pad, W A S D the left stick and
 * I J K L the right, X cross, C circle, Z square, V triangle, Q and E the
 * shoulder buttons, Enter START and Shift (or Backspace) SELECT.
 *
 * A controller is read as well, when there is one: the first that XInput
 * reports. Its buttons are added to the keyboard's and its sticks are the
 * sticks wherever the keyboard is not holding one. The library is looked
 * for by name when the window opens, so the host links nothing for it and
 * runs the same where it is absent. Like the keys, a pad is listened to
 * only while the window has the focus.
 */

#include "window.h"
#include "vitarecomp/host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct {
    HWND             hwnd;
    HANDLE           thread, ready;
    CRITICAL_SECTION lock;
    uint32_t        *frame;             /* B, G, R, x: what StretchDIBits reads */
    int              fw, fh;            /* the frame's size */
    volatile LONG    drawn;             /* someone else draws the picture: nothing is painted here */
    int              ww, wh;            /* the client size asked for */
    volatile LONG    buttons, closed;
    volatile LONG    finger;            /* bit 31 while the button is held; y above x, eleven bits each */
    volatile LONG    tilt;              /* the eight stick keys held, a bit each */
    volatile LONG    focused;
    char             title[256];
    LARGE_INTEGER    freq, next;
} W;

static const struct { WPARAM key; uint32_t bit; } KEYS[] = {
    { VK_UP, VITA_CTRL_UP },       { VK_DOWN, VITA_CTRL_DOWN },   { VK_LEFT, VITA_CTRL_LEFT },
    { VK_RIGHT, VITA_CTRL_RIGHT }, { 'X', VITA_CTRL_CROSS },      { 'C', VITA_CTRL_CIRCLE },
    { 'Z', VITA_CTRL_SQUARE },     { 'V', VITA_CTRL_TRIANGLE },   { VK_RETURN, VITA_CTRL_START },
    { VK_BACK, VITA_CTRL_SELECT }, { VK_SHIFT, VITA_CTRL_SELECT },
    { 'Q', VITA_CTRL_LTRIGGER },   { 'E', VITA_CTRL_RTRIGGER },
};

/* The stick keys: left, right, up, down of the left stick, then of the right. */
static const WPARAM TILTS[8] = { 'A', 'D', 'W', 'S', 'J', 'L', 'I', 'K' };

/* --- a controller --------------------------------------------------------------------
 * XInput's state of a pad, as its header gives it; declared here so that
 * nothing of the library is needed to build. */
typedef struct { WORD buttons; BYTE left_trigger, right_trigger; SHORT lx, ly, rx, ry; } pad_buttons;
typedef struct { DWORD packet; pad_buttons pad; } pad_state;
typedef DWORD (WINAPI *pad_get_state)(DWORD, pad_state *);

static struct {
    pad_get_state get;
    int           looked;               /* for the library */
    int           have, slot;           /* a pad answered, and which */
    DWORD         read_at, look_at;     /* when it was last read; when to look for one again */
    pad_buttons   now;
} P;

static const struct { WORD pad; uint32_t bit; } PAD_BUTTONS[] = {
    { 0x0001, VITA_CTRL_UP },       { 0x0002, VITA_CTRL_DOWN },     { 0x0004, VITA_CTRL_LEFT },
    { 0x0008, VITA_CTRL_RIGHT },    { 0x0010, VITA_CTRL_START },    { 0x0020, VITA_CTRL_SELECT },
    { 0x0040, VITA_CTRL_L3 },       { 0x0080, VITA_CTRL_R3 },       { 0x0100, VITA_CTRL_LTRIGGER },
    { 0x0200, VITA_CTRL_RTRIGGER }, { 0x1000, VITA_CTRL_CROSS },    { 0x2000, VITA_CTRL_CIRCLE },
    { 0x4000, VITA_CTRL_SQUARE },   { 0x8000, VITA_CTRL_TRIANGLE },
};

/* The pad as it is now. Read at most every other millisecond, and when
 * none answers, looked for again once a second: asking after a pad that is
 * not there is slow. */
static const pad_buttons *pad_read(void) {
    static const pad_buttons none;
    DWORD t = GetTickCount();
    if (!P.looked) {
        static const char *const NAMES[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        P.looked = 1;
        for (int i = 0; i < 3 && !P.get; i++) {
            HMODULE lib = LoadLibraryA(NAMES[i]);
            if (lib) P.get = (pad_get_state)(void *)GetProcAddress(lib, "XInputGetState");
        }
    }
    if (!P.get || !W.focused) return &none;
    if (P.have && t - P.read_at < 2) return &P.now;
    if (!P.have && (LONG)(t - P.look_at) < 0) return &none;
    pad_state st;
    for (int i = 0; i < 4; i++) {
        int slot = P.have ? P.slot : i;
        memset(&st, 0, sizeof(st));
        if (P.get((DWORD)slot, &st) == ERROR_SUCCESS) {
            P.now = st.pad; P.slot = slot; P.have = 1; P.read_at = t;
            return &P.now;
        }
        if (P.have) break;              /* the one there was has gone */
    }
    P.have = 0;
    P.look_at = t + 1000;
    return &none;
}

/* A stick of the pad as the console reports one: 128 at rest, 0 left or up.
 * Inside the ring a stick does not leave at rest, nothing; outside it, from
 * nothing at the ring to all the way at the edge. */
static void pad_stick(SHORT sx, SHORT sy, int dead, uint8_t *x, uint8_t *y) {
    double fx = (double)sx, fy = -(double)sy, reach = sqrt(fx * fx + fy * fy);
    *x = *y = 128;
    if (reach <= (double)dead) return;
    double scale = (reach - dead) / (32767.0 - dead) / reach;
    if (scale * reach > 1.0) scale = 1.0 / reach;
    int vx = 128 + (int)lround(fx * scale * 127.0), vy = 128 + (int)lround(fy * scale * 127.0);
    *x = (uint8_t)(vx < 0 ? 0 : vx > 255 ? 255 : vx);
    *y = (uint8_t)(vy < 0 ? 0 : vy > 255 ? 255 : vy);
}

static void key(WPARAM k, int down) {
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (KEYS[i].key != k) continue;
        if (down) InterlockedOr(&W.buttons, (LONG)KEYS[i].bit);
        else      InterlockedAnd(&W.buttons, ~(LONG)KEYS[i].bit);
    }
    for (int i = 0; i < 8; i++) {
        if (TILTS[i] != k) continue;
        if (down) InterlockedOr(&W.tilt, 1L << i);
        else      InterlockedAnd(&W.tilt, ~(1L << i));
    }
}

/* Where in the client area the picture is: as large as fits at the frame's
 * shape, in the middle. */
static void picture(HWND h, RECT *p) {
    RECT rc;
    GetClientRect(h, &rc);
    LONGLONG fw = W.fw > 0 ? W.fw : 960, fh = W.fh > 0 ? W.fh : 544;
    LONG w = rc.right, t = rc.bottom;
    if (w * fh > t * fw) w = (LONG)(t * fw / fh);
    else                 t = (LONG)(w * fh / fw);
    p->left = (rc.right - w) / 2;
    p->top = (rc.bottom - t) / 2;
    p->right = p->left + w;
    p->bottom = p->top + t;
}

/* The mouse at a client position, as a place on the panel. */
static void finger(HWND h, LPARAM lp) {
    RECT rc;
    picture(h, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    int x = ((int)(short)LOWORD(lp) - rc.left) * 1920 / (rc.right - rc.left);
    int y = ((int)(short)HIWORD(lp) - rc.top) * 1088 / (rc.bottom - rc.top);
    if (x < 0) x = 0;
    if (x > 1919) x = 1919;
    if (y < 0) y = 0;
    if (y > 1087) y = 1087;
    InterlockedExchange(&W.finger, (LONG)(0x80000000u | ((uint32_t)y << 11) | (uint32_t)x));
}

/* Over the whole of the screen the window is on, without a frame; or back
 * to where and what it was. */
static void fullscreen(HWND h) {
    static WINDOWPLACEMENT before;
    DWORD style = (DWORD)GetWindowLongA(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        before.length = sizeof(before);
        if (!GetWindowPlacement(h, &before) || !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongA(h, GWL_STYLE, (LONG)(style & ~(DWORD)WS_OVERLAPPEDWINDOW));
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(h, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(h, &before);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

#define WM_WHOLE_SCREEN (WM_APP + 1)

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_WHOLE_SCREEN: fullscreen(h); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && !(lp & (1L << 30))) { fullscreen(h); return 0; }     /* Alt and Enter, not its repeat */
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) { if (!(lp & (1L << 30))) fullscreen(h); return 0; }
        key(wp, 1);
        return 0;
    case WM_KEYUP:   key(wp, 0); return 0;
    case WM_SETFOCUS: InterlockedExchange(&W.focused, 1); return 0;
    case WM_KILLFOCUS:
        InterlockedExchange(&W.focused, 0);
        InterlockedExchange(&W.buttons, 0);
        InterlockedExchange(&W.tilt, 0);
        return 0;
    case WM_LBUTTONDOWN: SetCapture(h); finger(h, lp); return 0;
    case WM_MOUSEMOVE: if (W.finger < 0) finger(h, lp); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: InterlockedExchange(&W.finger, 0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, pic;
        if (W.drawn) { EndPaint(h, &ps); return 0; }
        GetClientRect(h, &rc);
        EnterCriticalSection(&W.lock);
        picture(h, &pic);
        if (W.frame && W.fw > 0 && W.fh > 0) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = W.fw;
            bi.bmiHeader.biHeight = -W.fh;             /* top row first */
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, pic.left, pic.top, pic.right - pic.left, pic.bottom - pic.top, 0, 0, W.fw, W.fh,
                          W.frame, &bi, DIB_RGB_COLORS, SRCCOPY);
            /* what the picture leaves, at the sides or above and below */
            RECT bar = rc;
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            if (pic.left > 0) { bar.right = pic.left; FillRect(dc, &bar, black); bar = rc; bar.left = pic.right; FillRect(dc, &bar, black); }
            if (pic.top > 0)  { bar = rc; bar.bottom = pic.top; FillRect(dc, &bar, black); bar = rc; bar.top = pic.bottom; FillRect(dc, &bar, black); }
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        LeaveCriticalSection(&W.lock);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        InterlockedExchange(&W.closed, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID arg) {
    (void)arg;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(101));     /* the program's own, if it has one */
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "vitarecomp_host";
    RegisterClassA(&wc);

    RECT rc = { 0, 0, W.ww, W.wh };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rc, style, FALSE);
    W.hwnd = CreateWindowA(wc.lpszClassName, W.title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    if (W.hwnd) ShowWindow(W.hwnd, SW_SHOWNORMAL);
    SetEvent(W.ready);
    if (!W.hwnd) return 1;

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    InterlockedExchange(&W.closed, 1);
    return 0;
}

/* Tell the system that sizes here are in the screen's own pixels. The call
 * is looked for by name: a system from before screens differed has the
 * older one, which says the same of the first screen only. */
static void own_pixels(void) {
    typedef BOOL (WINAPI *set_context)(HANDLE);
    HMODULE user = GetModuleHandleA("user32.dll");
    set_context set = user ? (set_context)(void *)GetProcAddress(user, "SetProcessDpiAwarenessContext") : NULL;
    if (!set || !set((HANDLE)(LONG_PTR)-4))         /* each screen by its own measure, as the system's header numbers it */
        SetProcessDPIAware();
}

int vita_window_open(const char *title, int width, int height) {
    if (W.thread) return 0;
    own_pixels();
    /* a sleep ends when it was asked to, to the millisecond: without this the system rounds one up to its own tick,
     * which is most of a sixtieth of a second */
    timeBeginPeriod(1);
    InitializeCriticalSection(&W.lock);
    W.ww = width;
    W.wh = height;
    strncpy(W.title, title ? title : "vita_host", sizeof(W.title) - 1);
    QueryPerformanceFrequency(&W.freq);
    QueryPerformanceCounter(&W.next);
    W.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    W.thread = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (!W.thread) return -1;
    WaitForSingleObject(W.ready, INFINITE);
    return W.hwnd ? 0 : -1;
}

void vita_window_present(const uint8_t *pixels, int width, int height, int pitch) {
    if (!W.hwnd || !pixels || width <= 0 || height <= 0) return;
    EnterCriticalSection(&W.lock);
    if (W.fw != width || W.fh != height || !W.frame) {
        free(W.frame);
        W.frame = (uint32_t *)malloc((size_t)width * (size_t)height * 4);
        W.fw = width;
        W.fh = height;
    }
    if (W.frame) {
        for (int y = 0; y < height; y++) {
            const uint8_t *s = pixels + (size_t)y * (size_t)pitch * 4;
            uint32_t *d = W.frame + (size_t)y * (size_t)width;
            for (int x = 0; x < width; x++, s += 4)
                d[x] = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | (uint32_t)s[2];
        }
    }
    LeaveCriticalSection(&W.lock);
    InvalidateRect(W.hwnd, NULL, FALSE);
}

void *vita_window_native(void) { return W.hwnd; }

void vita_window_drawn(int width, int height) {
    if (width > 0 && height > 0 && (W.fw != width || W.fh != height)) {
        EnterCriticalSection(&W.lock);
        free(W.frame);
        W.frame = NULL;
        W.fw = width;
        W.fh = height;
        LeaveCriticalSection(&W.lock);
    }
    InterlockedExchange(&W.drawn, 1);
}

void vita_window_fullscreen(void) {
    if (W.hwnd) PostMessageA(W.hwnd, WM_WHOLE_SCREEN, 0, 0);
}

uint32_t vita_window_buttons(void) {
    const pad_buttons *pad = pad_read();
    uint32_t held = (uint32_t)W.buttons;
    for (size_t i = 0; i < sizeof(PAD_BUTTONS) / sizeof(PAD_BUTTONS[0]); i++)
        if (pad->buttons & PAD_BUTTONS[i].pad) held |= PAD_BUTTONS[i].bit;
    /* the console has one shoulder button a side: a pad's trigger is the same button */
    if (pad->left_trigger > 30) held |= VITA_CTRL_LTRIGGER;
    if (pad->right_trigger > 30) held |= VITA_CTRL_RTRIGGER;
    return held;
}

void vita_window_sticks(uint8_t xy[4]) {
    const pad_buttons *pad = pad_read();
    LONG held = W.tilt;
    pad_stick(pad->lx, pad->ly, 7849, &xy[0], &xy[1]);       /* the rings XInput's header gives its sticks */
    pad_stick(pad->rx, pad->ry, 8689, &xy[2], &xy[3]);
    for (int axis = 0; axis < 4; axis++) {
        int low = (held >> (2 * axis)) & 1, high = (held >> (2 * axis + 1)) & 1;
        if (low != high) xy[axis] = (uint8_t)(low ? 0 : 255);  /* a key held is the stick, whatever the pad says */
    }
}

int vita_window_touch(int *x, int *y) {
    uint32_t f = (uint32_t)W.finger;
    if (!(f & 0x80000000u)) return 0;
    *x = (int)(f & 0x7FF);
    *y = (int)((f >> 11) & 0x7FF);
    return 1;
}
int      vita_window_closed(void)  { return W.closed != 0; }

void vita_window_title(const char *text) {
    if (W.hwnd && text) SetWindowTextA(W.hwnd, text);
}

uint32_t vita_window_pace_left(void) {
    LARGE_INTEGER now;
    if (!W.freq.QuadPart) return 0;
    QueryPerformanceCounter(&now);
    LONGLONG left = W.next.QuadPart + W.freq.QuadPart / 60 - now.QuadPart;
    return left > 0 ? (uint32_t)(left * 1000000 / W.freq.QuadPart) : 0;
}

void vita_window_pace(void) {
    if (!W.freq.QuadPart) return;
    LONGLONG step = W.freq.QuadPart / 60;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    W.next.QuadPart += step;
    if (W.next.QuadPart < now.QuadPart - step * 4) W.next.QuadPart = now.QuadPart;    /* fell behind: do not chase */
    while (now.QuadPart < W.next.QuadPart) {
        LONGLONG left_ms = (W.next.QuadPart - now.QuadPart) * 1000 / W.freq.QuadPart;
        Sleep(left_ms > 2 ? (DWORD)(left_ms - 1) : 0);
        QueryPerformanceCounter(&now);
    }
}
