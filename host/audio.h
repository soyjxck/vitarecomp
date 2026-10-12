/* audio.h — the host's sound device.
 *
 * The runtime keeps what the program plays in queues at one rate
 * (hle_audio.c); a device, when there is one, takes their sum
 * (vita_audio_pull) on a thread of its own and tells the runtime it is
 * there (vita_audio_device), from which moment a program's blocks are
 * queued and paced by what the device has taken. One implementation per
 * platform: audio_win32.c (the system's wave output), audio_none.c.
 */

#ifndef VITARECOMP_HOST_AUDIO_H
#define VITARECOMP_HOST_AUDIO_H

/* Open the default output and start feeding it. 0 when sound is going out;
 * nonzero when there is no device, and the run goes on without. */
int vita_host_audio_start(void);

/* Stop and close it. Safe to call when none was started. */
void vita_host_audio_stop(void);

#endif /* VITARECOMP_HOST_AUDIO_H */
