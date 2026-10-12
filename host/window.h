/* window.h — a window for the host, when it has one.
 *
 * The host is headless by default: scripted input, frames to files, the same
 * in CI as on a desk. With --window it also shows what the program displays
 * and reads the keyboard and the mouse. The window lives on a thread of its own, because
 * guest threads are host threads and whichever of them waits for the
 * vertical blank is the one that presents: a window must be pumped by the
 * thread that made it, and no guest thread can promise to be that one.
 */

#ifndef VITARECOMP_HOST_WINDOW_H
#define VITARECOMP_HOST_WINDOW_H

#include <stdint.h>

/* Open the window, its picture `width` by `height` pixels. The picture
 * keeps the frame's shape whatever shape the window is given afterwards:
 * what is over is black. Returns 0, or -1 when this build has no window. */
int vita_window_open(const char *title, int width, int height);

/* The window over the whole screen, or back to what it was: what F11 does. */
void vita_window_fullscreen(void);

/* Show a frame: `pixels` is R, G, B, A bytes, `pitch` pixels to a row. */
void vita_window_present(const uint8_t *pixels, int width, int height, int pitch);

/* The window as the system names it, an HWND on Windows, for whoever would
 * draw the picture in it for themselves: a graphics card that holds the
 * frame already. NULL when there is no window. */
void *vita_window_native(void);

/* Say that someone has, a frame `width` by `height`: the window paints
 * nothing of its own from here on, and places the mouse on a picture of
 * that shape. */
void vita_window_drawn(int width, int height);

/* The buttons the keyboard and a controller are holding, as a VITA_CTRL_*
 * mask. */
uint32_t vita_window_buttons(void);

/* The sticks: a controller's where there is one, and as the keyboard holds
 * them wherever it does, W A S D the left one, I J K L the right, each all
 * the way over. In host.h's form. */
void vita_window_sticks(uint8_t xy[4]);

/* Whether the left mouse button is held on the picture, and where: as a
 * finger on a panel 1920 across and 1088 down, whatever size the picture is. */
int vita_window_touch(int *x, int *y);

/* Whether the window has been closed. */
int vita_window_closed(void);

/* Replace the title bar's text. */
void vita_window_title(const char *text);

/* Sleep until the next sixtieth of a second since the last call. */
void vita_window_pace(void);

/* How long that sleep would be if it began now, in microseconds: for a host
 * that has something better to do with most of it. */
uint32_t vita_window_pace_left(void);

#endif /* VITARECOMP_HOST_WINDOW_H */
