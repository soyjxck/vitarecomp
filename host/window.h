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

/* Open the window. Returns 0, or -1 when this build has no window. */
int vita_window_open(const char *title, int width, int height);

/* Show a frame: `pixels` is R, G, B, A bytes, `pitch` pixels to a row. */
void vita_window_present(const uint8_t *pixels, int width, int height, int pitch);

/* The buttons the keyboard is holding, as a VITA_CTRL_* mask. */
uint32_t vita_window_buttons(void);

/* Whether the left mouse button is held on the picture, and where: as a
 * finger on a panel 1920 across and 1088 down, whatever size the window is. */
int vita_window_touch(int *x, int *y);

/* Whether the window has been closed. */
int vita_window_closed(void);

/* Replace the title bar's text. */
void vita_window_title(const char *text);

/* Sleep until the next sixtieth of a second since the last call. */
void vita_window_pace(void);

#endif /* VITARECOMP_HOST_WINDOW_H */
