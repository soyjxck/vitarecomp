/* audio_none.c — no sound device: a host with nothing to play through. */

#include "audio.h"

int vita_host_audio_start(void) { return 1; }
void vita_host_audio_stop(void) { }
