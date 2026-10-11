/* window_win32.c — the host's window on Windows: GDI and nothing else.
 *
 * One thread owns the window and its message loop. Presenting a frame copies
 * it, swapped into the byte order a DIB wants, into a buffer the paint
 * handler stretches over the client area; the keyboard is tracked from the
 * window's own key messages, so it reads nothing while another window has
 * the focus.
 */

#include "window.h"
#include "vitarecomp/host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

static struct {
    HWND             hwnd;
    HANDLE           thread, ready;
    CRITICAL_SECTION lock;
    uint32_t        *frame;             /* B, G, R, x: what StretchDIBits reads */
    int              fw, fh;            /* the frame's size */
    int              ww, wh;            /* the client size asked for */
    volatile LONG    buttons, closed;
    char             title[256];
    LARGE_INTEGER    freq, next;
} W;

static const struct { WPARAM key; uint32_t bit; } KEYS[] = {
    { VK_UP, VITA_CTRL_UP },       { VK_DOWN, VITA_CTRL_DOWN },   { VK_LEFT, VITA_CTRL_LEFT },
    { VK_RIGHT, VITA_CTRL_RIGHT }, { 'Z', VITA_CTRL_CROSS },      { 'X', VITA_CTRL_CIRCLE },
    { 'A', VITA_CTRL_SQUARE },     { 'S', VITA_CTRL_TRIANGLE },   { VK_RETURN, VITA_CTRL_START },
    { VK_BACK, VITA_CTRL_SELECT }, { 'Q', VITA_CTRL_LTRIGGER },   { 'W', VITA_CTRL_RTRIGGER },
};

static void key(WPARAM k, int down) {
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (KEYS[i].key != k) continue;
        if (down) InterlockedOr(&W.buttons, (LONG)KEYS[i].bit);
        else      InterlockedAnd(&W.buttons, ~(LONG)KEYS[i].bit);
    }
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: key(wp, 1); return 0;
    case WM_KEYUP:   key(wp, 0); return 0;
    case WM_KILLFOCUS: InterlockedExchange(&W.buttons, 0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        EnterCriticalSection(&W.lock);
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
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, W.fw, W.fh, W.frame, &bi, DIB_RGB_COLORS, SRCCOPY);
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

int vita_window_open(const char *title, int width, int height) {
    if (W.thread) return 0;
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

uint32_t vita_window_buttons(void) { return (uint32_t)W.buttons; }
int      vita_window_closed(void)  { return W.closed != 0; }

void vita_window_title(const char *text) {
    if (W.hwnd && text) SetWindowTextA(W.hwnd, text);
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
