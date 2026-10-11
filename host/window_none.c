/* window_none.c — the host's window on a platform that has not been given
 * one yet: there is none, and --window says so. */

#include "window.h"

#include <stddef.h>

int      vita_window_open(const char *title, int width, int height) { (void)title; (void)width; (void)height; return -1; }
void     vita_window_fullscreen(void) { }
void     vita_window_present(const uint8_t *pixels, int width, int height, int pitch) { (void)pixels; (void)width; (void)height; (void)pitch; }
uint32_t vita_window_buttons(void) { return 0; }
int      vita_window_touch(int *x, int *y) { (void)x; (void)y; return 0; }
void     vita_window_sticks(uint8_t xy[4]) { xy[0] = xy[1] = xy[2] = xy[3] = 128; }
int      vita_window_closed(void) { return 0; }
void     vita_window_title(const char *text) { (void)text; }
void     vita_window_pace(void) { }
