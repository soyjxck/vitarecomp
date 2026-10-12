/* hle_avplayer.c — SceAvPlayer: the console's movie player.
 *
 * A program makes a player, names a file for it, and from then on asks it
 * each frame for the picture that is due and, if it wants the movie's own
 * sound, for the next block of that; it draws the one and plays the other
 * itself. The player never touches the screen or an audio port.
 *
 * What a program hands the player when it makes it (SceAvPlayerInitData):
 *
 *   memory   allocate and deallocate for the player's own records, and a
 *            second pair for the pictures, which a program takes from
 *            memory the graphics library can see: it draws a picture by
 *            pointing a texture at the bytes the player gave it
 *   files    open, close, read at an offset and size: when all four are
 *            given the player reads the movie through them and never opens
 *            anything itself
 *   events   one function, told each time the player changes state
 *
 * All of them are the program's code and are called as such (call_guest).
 *
 * The decoding is somebody else's (movie.h). What is here is the console's
 * interface over it, and three things about that interface are worth
 * saying, because a title's code leans on each:
 *
 * The source is opened in the call that names it. A file the decoder
 * cannot play, or a build with no decoder at all, is refused there, where
 * a title looks: told yes, it waits for the player to say it is ready, and
 * nothing would ever say so. That is the one place this leaves the
 * console's order of things, where the opening is done by the player's own
 * thread after the call has returned.
 *
 * The events come from a thread of the player's own, afterwards. A title
 * typically adds its source, sets a flag, and sleeps until its event
 * function has cleared the flag: an event delivered inside the call that
 * added the source would clear the flag before it was set, and the title
 * would sleep for ever. So the player has a thread, as it has on the
 * console (the init data's priority is that thread's), which does nothing
 * but wait for something to tell the program and tell it, one event at a
 * time and never one inside another. The first event of a source is held
 * back a moment, the time the console takes to look at a file.
 *
 * The pictures keep the movie's time by the clock, not by how often they
 * are asked for. A program's own music may be playing beside the movie
 * (the movies of the title this was written for carry silence, and the
 * title plays a track of its own over them), so a picture is handed out
 * when the clock (clock.h) has reached its time and one that the program
 * was too slow to ask for is passed over. A picture is in the layout the
 * console's decoder leaves and movie.h describes: a plane of brightness and
 * a plane of colour differences in pairs, which a title draws through the
 * texture format made for it (gxm_tex.h).
 *
 * What is not here: a second source queued behind the first (a player has
 * one, until it is closed), jumping to a time, speeds other than the
 * movie's own, and timed text. The calls for the last three have no
 * function here and stay the named traps they were.
 *
 * The states and their numbers are the SDK's; the order of the events
 * around a source (buffering, then ready) is the Vita3K project's reading
 * of the console. Calling convention: see hle.h.
 */

#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "clock.h"
#include "hle_fios.h"
#include "hle_sysmem.h"
#include "movie.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_AVPLAYER_ERROR_INVALID_PARAM 0x806A0001u
#define SCE_AVPLAYER_ERROR_OUT_OF_MEMORY 0x806A0003u
#define SCE_AVPLAYER_ERROR_NOT_SUPPORTED 0x806A0004u

/* The player's states, which are also the numbers its events carry. */
enum { STATE_NONE = 0, STATE_STOP = 1, STATE_READY = 2, STATE_PLAY = 3, STATE_PAUSE = 4, STATE_BUFFERING = 5 };

#define PLAYERS       4
#define MOST_PICTURES 8                 /* buffers a picture is handed out in, in turn */
#define SOUND_BLOCKS  4
#define SOUND_FRAMES  1024u             /* a block of sound: what one frame of AAC decodes to */
#define SOUND_AHEAD   500000u           /* how far before its time sound is handed out, in microseconds */
#define READ_BYTES    0x20000u          /* what the program's read function is asked for at a time */
#define SETTLE_US     1000u             /* how long after a source is added its first event comes */
#define THREAD_STACK  0x10000u
#define EVENTS        16

typedef struct {
    int      used;
    uint32_t handle;                    /* what the program knows the player by: a record in memory of its own giving */

    /* what the program handed over */
    uint32_t mem_arg, allocate, deallocate, allocate_picture, deallocate_picture;
    uint32_t file_arg, file_open, file_close, file_read, file_size;
    uint32_t event_arg, event_function;
    uint32_t priority, pictures_wanted, auto_start;

    /* the source */
    movie          *film;
    vita_fios_file *own_file;           /* the file, when the player reads it itself */
    int             program_file;       /* the program's open was called, and its close is owed */
    uint64_t        file_bytes;
    uint32_t        read_into;          /* memory of the program's for its read function to fill */
    uint32_t        enabled;            /* a bit a stream */
    int             video, audio;       /* the streams the movie decodes, or -1 */
    uint32_t        width, height;

    /* pictures: the one decoded and waiting for its time, and the buffers they are handed out in */
    uint8_t *waiting;
    int      is_waiting, video_over;
    uint64_t waiting_time, waiting_shown;
    uint32_t picture[MOST_PICTURES];
    uint32_t pictures, picture_next;
    uint32_t handed_out, passed_over;   /* for the trace: pictures the program had, and those it was too late for */

    /* sound */
    uint32_t sound[SOUND_BLOCKS];
    uint32_t sound_next;
    uint64_t sound_time;                /* when the next block to be handed out is due */
    int      audio_over;

    /* time */
    int      state, looping;
    uint64_t began;                     /* the clock when the movie's time was nothing */
    uint64_t paused;                    /* the clock when it was paused */
    uint64_t ends;                      /* the movie's time when the last of it has been shown */

    /* the thread that tells the program things, and what it has yet to tell */
    uint32_t thread;
    int      settle, closing;
    uint32_t queued;
    int32_t  queue[EVENTS];
} player;

static player g_players[PLAYERS];

static player *by_handle(uint32_t handle) {
    for (int i = 0; i < PLAYERS; i++)
        if (g_players[i].used && g_players[i].handle == handle && handle) return &g_players[i];
    return NULL;
}

/* --- the program's functions ------------------------------------------------------------
 * Called as one of its own functions calls another: arguments in the first
 * four registers and, for the one that takes more, on the stack; the answer
 * in the first register, or the first two. The registers a call may
 * destroy are the ones the program expects the player's function to have
 * kept, since to it that is one call, so they are put back. */
static uint32_t call_guest(uint32_t function, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, const uint32_t *fifth, uint32_t *high) {
    const uint32_t s0 = r0, s1 = r1, s2 = r2, s3 = r3, s12 = r12, slr = lr, ssp = sp;
    if (fifth) {
        sp -= 8;
        vita_write32(sp, *fifth);
        vita_write32(sp + 4, 0);
    }
    r0 = a0; r1 = a1; r2 = a2; r3 = a3;
    vita_dispatch(0, function);
    const uint32_t low = r0;
    if (high) *high = r1;
    sp = ssp;
    r0 = s0; r1 = s1; r2 = s2; r3 = s3; r12 = s12; lr = slr;
    return low;
}

/* Memory for the player, of the program's giving when it gave the means:
 * void *allocate(void *arg, uint32_t alignment, uint32_t size). A program
 * that gave none gets pages of the runtime's that are never given back. */
static uint32_t take(const player *p, int picture, uint32_t alignment, uint32_t size) {
    const uint32_t function = picture ? p->allocate_picture : p->allocate;
    if (!function) return vita_sysmem_reserve(size);
    const uint32_t at = call_guest(function, p->mem_arg, alignment, size, 0, NULL, NULL);
    return at && vita_mem_ptr(at, size) ? at : 0;
}

/* void deallocate(void *arg, void *memory) */
static void give_back(const player *p, int picture, uint32_t at) {
    const uint32_t function = picture ? p->deallocate_picture : p->deallocate;
    if (at && function) (void)call_guest(function, p->mem_arg, at, 0, 0, NULL, NULL);
}

/* --- the file ---------------------------------------------------------------------------- */

/* The movie's reader (movie.h): through the program's read function, a
 * piece at a time into memory of the program's and from there to where the
 * bytes are wanted, or from the file the player opened itself.
 *
 * int readOffset(void *arg, uint8_t *buffer, uint64_t position, uint32_t length):
 * the position is sixty-four bits and so takes the third and fourth
 * registers, and the length goes on the stack. It is never asked for bytes
 * past the file's end: a program's function may answer a read it could
 * not fill with nothing at all. */
static int64_t read_movie(void *user, uint64_t offset, void *dst, uint32_t length) {
    player *p = (player *)user;
    if (offset >= p->file_bytes) return 0;
    if (length > p->file_bytes - offset) length = (uint32_t)(p->file_bytes - offset);
    if (p->own_file) return vita_fios_read(p->own_file, offset, dst, length);
    uint32_t done = 0;
    while (done < length) {
        const uint32_t want = length - done < READ_BYTES ? length - done : READ_BYTES;
        const uint64_t at = offset + done;
        const int32_t got = (int32_t)call_guest(p->file_read, p->file_arg, p->read_into, (uint32_t)at, (uint32_t)(at >> 32), &want, NULL);
        const void *bytes = got > 0 && (uint32_t)got <= want ? vita_mem_ptr(p->read_into, (uint32_t)got) : NULL;
        if (!bytes) return done ? (int64_t)done : -1;
        memcpy((uint8_t *)dst + done, bytes, (uint32_t)got);
        done += (uint32_t)got;
        if ((uint32_t)got < want) break;
    }
    return (int64_t)done;
}

/* Open the file a source names: by the program's functions when it gave
 * all four, else by the path as the file library reads it. 0 when there is
 * no such file. */
static int open_file(player *p, uint32_t path_addr, const char *path) {
    if (p->file_open && p->file_close && p->file_read && p->file_size) {
        /* int open(void *arg, const char *filename); uint64_t size(void *arg) */
        const int32_t opened = (int32_t)call_guest(p->file_open, p->file_arg, path_addr, 0, 0, NULL, NULL);
        if (opened < 0) return 0;
        p->program_file = 1;
        uint32_t high = 0;
        const uint32_t low = call_guest(p->file_size, p->file_arg, 0, 0, 0, NULL, &high);
        p->file_bytes = ((uint64_t)high << 32) | low;
        p->read_into = take(p, 0, 0x40, READ_BYTES);
        return p->file_bytes != 0 && p->read_into != 0;
    }
    p->own_file = vita_fios_open(path);
    if (!p->own_file) return 0;
    p->file_bytes = vita_fios_size(p->own_file);
    return p->file_bytes != 0;
}

/* Let go of the source and everything made for it. */
static void drop_source(player *p) {
    movie_close(p->film);
    p->film = NULL;
    free(p->waiting);
    p->waiting = NULL;
    p->is_waiting = 0;
    for (uint32_t i = 0; i < p->pictures; i++) give_back(p, 1, p->picture[i]);
    p->pictures = 0;
    for (uint32_t i = 0; i < SOUND_BLOCKS; i++) { give_back(p, 0, p->sound[i]); p->sound[i] = 0; }
    give_back(p, 0, p->read_into);
    p->read_into = 0;
    /* int close(void *arg) */
    if (p->program_file) (void)call_guest(p->file_close, p->file_arg, 0, 0, 0, NULL, NULL);
    p->program_file = 0;
    vita_fios_close(p->own_file);
    p->own_file = NULL;
    p->enabled = 0;
    p->video = p->audio = -1;
}

/* --- events ------------------------------------------------------------------------------ */

/* Something for the program to be told, by the player's thread when it
 * next runs. */
static void tell(player *p, int32_t event) {
    if (!p->thread || p->queued == EVENTS) return;
    p->queue[p->queued++] = event;
    vita_sched_wake(VW_COND, p->handle, 1);
}

static void begin(player *p);
static void release(player *p);

/* The player's thread. It waits on the player's own record, which nothing
 * of a program's waits on, until there is an event to deliver or the
 * player is being closed; a player closed from inside its own event
 * function is taken apart here, once that function has returned.
 *
 * void eventCallback(void *arg, int32_t event, int32_t source, void *data) */
static void player_thread(void) {
    player *p = NULL;
    for (int i = 0; i < PLAYERS; i++)
        if (g_players[i].used && g_players[i].thread == vita_thread_self()) p = &g_players[i];
    while (p) {
        while (!p->queued && !p->closing) (void)vita_sched_block(VW_COND, p->handle, 0);
        if (!p->queued) break;
        if (p->settle) {
            p->settle = 0;
            vita_sched_delay(SETTLE_US);
            continue;
        }
        const int32_t event = p->queue[0];
        memmove(p->queue, p->queue + 1, (size_t)--p->queued * sizeof p->queue[0]);
        vita_hle_trace("sceAvPlayer: event %d to 0x%08X", (int)event, p->event_function);
        if (p->event_function) (void)call_guest(p->event_function, p->event_arg, (uint32_t)event, 0, 0, NULL, NULL);
        if (event == STATE_READY && p->auto_start && p->state == STATE_READY && !p->closing) begin(p);
    }
    if (p && p->closing == 2) release(p);
    r0 = 0;
}

/* --- time -------------------------------------------------------------------------------- */

/* The movie's time now, in microseconds. */
static uint64_t movie_now(const player *p) {
    if (p->state == STATE_PAUSE) return p->paused - p->began;
    if (p->state != STATE_PLAY) return 0;
    const uint64_t now = vita_clock_us();
    return now > p->began ? now - p->began : 0;
}

/* Start playing from the beginning: the streams the program enabled, or
 * when it enabled none the first of pictures and the first of sound. */
static void begin(player *p) {
    if (!p->film || p->closing) return;
    if (!p->enabled) {
        if (p->video >= 0) p->enabled |= 1u << p->video;
        if (p->audio >= 0) p->enabled |= 1u << p->audio;
    }
    p->ends = 0;
    for (uint32_t k = 0; k < movie_streams(p->film); k++) {
        const movie_stream *s = movie_stream_at(p->film, k);
        if (((p->enabled >> k) & 1) && s->decoded && s->duration_us > p->ends) p->ends = s->duration_us;
    }
    p->began = vita_clock_us();
    p->sound_time = 0;
    p->state = STATE_PLAY;
    vita_hle_trace("sceAvPlayer: playing, streams 0x%X, %llu ms", p->enabled, (unsigned long long)(p->ends / 1000));
    tell(p, STATE_PLAY);
}

/* Whether the movie has run out, and what follows if it has: round again
 * for one set to loop, else the player stops and says so. Returns the
 * movie's time. */
static uint64_t keep_time(player *p) {
    uint64_t now = movie_now(p);
    if (p->state != STATE_PLAY || now < p->ends) return now;
    if (p->looping && p->ends && movie_rewind(p->film)) {
        p->began += p->ends;
        p->is_waiting = 0;
        p->video_over = p->audio_over = 0;
        p->sound_time = 0;
        return movie_now(p);
    }
    vita_hle_trace("sceAvPlayer: the movie is over at %llu ms", (unsigned long long)(now / 1000));
    p->state = STATE_STOP;
    tell(p, STATE_STOP);
    return now;
}

/* --- making and unmaking ------------------------------------------------------------------ */

/* SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *data)
 *
 * The init data: the four memory functions behind their argument, the four
 * file functions behind theirs, the event function behind its own, then a
 * debug level, the priority of the player's thread, how many buffers the
 * pictures are handed out in, and whether it starts by itself. */
void vita_hle_sceAvPlayerInit(void) {
    const uint32_t data = r0;
    player *p = NULL;
    for (int i = 0; i < PLAYERS && !p; i++) if (!g_players[i].used) p = &g_players[i];
    if (!p || !vita_mem_ptr(data, 0x40)) { r0 = p ? SCE_AVPLAYER_ERROR_INVALID_PARAM : SCE_AVPLAYER_ERROR_OUT_OF_MEMORY; return; }
    memset(p, 0, sizeof *p);
    p->mem_arg = vita_read32(data + 0x00);
    p->allocate = vita_read32(data + 0x04);
    p->deallocate = vita_read32(data + 0x08);
    p->allocate_picture = vita_read32(data + 0x0C);
    p->deallocate_picture = vita_read32(data + 0x10);
    p->file_arg = vita_read32(data + 0x14);
    p->file_open = vita_read32(data + 0x18);
    p->file_close = vita_read32(data + 0x1C);
    p->file_read = vita_read32(data + 0x20);
    p->file_size = vita_read32(data + 0x24);
    p->event_arg = vita_read32(data + 0x28);
    p->event_function = vita_read32(data + 0x2C);
    p->priority = vita_read32(data + 0x34);
    p->pictures_wanted = vita_read32(data + 0x38);
    p->auto_start = vita_read8(data + 0x3C);
    p->video = p->audio = -1;
    p->handle = take(p, 0, 0x10, 0x40);
    if (!p->handle) { r0 = SCE_AVPLAYER_ERROR_OUT_OF_MEMORY; return; }
    p->used = 1;
    vita_hle_trace("sceAvPlayerInit(0x%08X: memory 0x%08X 0x%08X 0x%08X 0x%08X, files 0x%08X 0x%08X 0x%08X 0x%08X, events 0x%08X, "
                   "priority 0x%X, %u picture buffers, auto start %u) -> 0x%08X", data, p->allocate, p->deallocate, p->allocate_picture,
                   p->deallocate_picture, p->file_open, p->file_close, p->file_read, p->file_size, p->event_function, p->priority,
                   p->pictures_wanted, p->auto_start, p->handle);
    r0 = p->handle;
}

/* int sceAvPlayerAddSource(SceAvPlayerHandle handle, const char *filename) */
void vita_hle_sceAvPlayerAddSource(void) {
    player *p = by_handle(r0);
    const uint32_t path_addr = r1;
    const char *path = vita_mem_avail(path_addr) ? (const char *)vita_mem_ptr(path_addr, 1) : NULL;
    char why[160] = "";
    if (!p || !path) { r0 = SCE_AVPLAYER_ERROR_INVALID_PARAM; return; }
    if (p->film) {
        vita_hle_trace("sceAvPlayerAddSource(\"%.200s\"): one source to a player, and it has one", path);
        r0 = SCE_AVPLAYER_ERROR_NOT_SUPPORTED;
        return;
    }

    movie_reader reader = { p, 0, read_movie };
    if (!open_file(p, path_addr, path)) snprintf(why, sizeof why, "no such file");
    reader.size = p->file_bytes;
    if (!why[0]) p->film = movie_open(&reader, why, sizeof why);
    if (!p->film) {
        vita_hle_trace("sceAvPlayerAddSource(\"%.200s\"): no movie is played: %s", path, why);
        drop_source(p);
        r0 = SCE_AVPLAYER_ERROR_NOT_SUPPORTED;
        return;
    }

    /* the streams it will decode, and for pictures the buffers they are handed out in */
    for (uint32_t k = 0; k < movie_streams(p->film); k++) {
        const movie_stream *s = movie_stream_at(p->film, k);
        if (s->decoded && s->kind == MOVIE_VIDEO) { p->video = (int)k; p->width = s->width; p->height = s->height; }
        if (s->decoded && s->kind == MOVIE_AUDIO) p->audio = (int)k;
    }
    int room = 1;
    if (p->video >= 0) {
        const size_t bytes = movie_picture_bytes(p->width, p->height);
        uint32_t count = p->pictures_wanted < 2 ? 2 : p->pictures_wanted > MOST_PICTURES ? MOST_PICTURES : p->pictures_wanted;
        p->waiting = (uint8_t *)malloc(bytes);
        if (p->waiting) {               /* black, for whatever of it a decoded picture turns out not to cover */
            memset(p->waiting, 16, (size_t)p->width * p->height);
            memset(p->waiting + (size_t)p->width * p->height, 128, bytes - (size_t)p->width * p->height);
        }
        for (; room && p->waiting && p->pictures < count; p->pictures++)
            room = (p->picture[p->pictures] = take(p, 1, 0x1000, (uint32_t)bytes)) != 0;
        room = room && p->waiting;
    }
    if (p->audio >= 0) {
        const uint32_t bytes = SOUND_FRAMES * movie_stream_at(p->film, (uint32_t)p->audio)->channels * 2;
        for (uint32_t i = 0; room && i < SOUND_BLOCKS; i++) room = (p->sound[i] = take(p, 0, 0x40, bytes)) != 0;
    }
    /* the thread that will say so, made when a player first has something to say */
    if (room && !p->thread) {
        p->thread = vita_thread_create("SceAvPlayer", vita_dispatch_host(player_thread), p->priority ? p->priority : 0xA0, THREAD_STACK);
        if (!p->thread || vita_thread_start(p->thread, 0, 0) != 0) {
            if (p->thread) vita_thread_delete(p->thread);
            p->thread = 0;
            room = 0;
        }
    }
    if (!room) {
        vita_hle_trace("sceAvPlayerAddSource(\"%.200s\"): no memory or no thread for it", path);
        drop_source(p);
        r0 = SCE_AVPLAYER_ERROR_OUT_OF_MEMORY;
        return;
    }

    p->video_over = p->audio_over = 0;
    p->picture_next = p->sound_next = 0;
    p->state = STATE_READY;
    p->settle = 1;
    tell(p, STATE_BUFFERING);
    tell(p, STATE_READY);
    vita_hle_trace("sceAvPlayerAddSource(\"%.200s\"): %llu bytes read %s, %u streams, pictures %ux%u in %u buffers", path,
                   (unsigned long long)p->file_bytes, p->own_file ? "by the player" : "through the program's functions",
                   movie_streams(p->film), p->width, p->height, p->pictures);
    r0 = 0;
}

/* Everything of a player's given back, and the player forgotten. */
static void release(player *p) {
    const uint32_t handle = p->handle;
    drop_source(p);
    give_back(p, 0, handle);
    p->used = 0;
}

/* int sceAvPlayerClose(SceAvPlayerHandle handle): the program hears that
 * the player has stopped, if it was doing anything, before the call
 * returns: by then it may have thrown away what its event function uses. */
void vita_hle_sceAvPlayerClose(void) {
    player *p = by_handle(r0);
    if (!p) { r0 = 0; return; }
    vita_hle_trace("sceAvPlayerClose(0x%08X): %u pictures were handed out, %u passed over", p->handle, p->handed_out, p->passed_over);
    /* of what it has not been told yet, only that the player stopped is still of use to it */
    int stopped = p->state != STATE_NONE && p->state != STATE_STOP;
    for (uint32_t k = 0; k < p->queued; k++) stopped |= p->queue[k] == STATE_STOP;
    p->queued = 0;
    p->settle = 0;
    p->state = STATE_STOP;
    if (stopped) tell(p, STATE_STOP);
    if (p->thread && p->thread == vita_thread_self()) {
        p->closing = 2;                 /* from inside its own event function: its thread finishes the job */
        r0 = 0;
        return;
    }
    if (p->thread) {
        p->closing = 1;
        vita_sched_wake(VW_COND, p->handle, 1);
        (void)vita_thread_wait_end(p->thread, 0, NULL);
        (void)vita_thread_delete(p->thread);
        p->thread = 0;
    }
    release(p);
    r0 = 0;
}

/* --- playing ------------------------------------------------------------------------------ */

/* int sceAvPlayerStart(SceAvPlayerHandle handle) */
void vita_hle_sceAvPlayerStart(void) {
    player *p = by_handle(r0);
    if (!p || !p->film) { r0 = SCE_AVPLAYER_ERROR_INVALID_PARAM; return; }
    if (p->state == STATE_STOP) {       /* again, after it stopped: from the beginning */
        (void)movie_rewind(p->film);
        p->is_waiting = 0;
        p->video_over = p->audio_over = 0;
    }
    if (p->state != STATE_PLAY && p->state != STATE_PAUSE) begin(p);
    r0 = 0;
}

/* int sceAvPlayerStop(SceAvPlayerHandle handle) */
void vita_hle_sceAvPlayerStop(void) {
    player *p = by_handle(r0);
    if (p && (p->state == STATE_PLAY || p->state == STATE_PAUSE)) { p->state = STATE_STOP; tell(p, STATE_STOP); }
    r0 = 0;
}

/* int sceAvPlayerPause(SceAvPlayerHandle handle) */
void vita_hle_sceAvPlayerPause(void) {
    player *p = by_handle(r0);
    if (p && p->state == STATE_PLAY) {
        p->paused = vita_clock_us();
        p->state = STATE_PAUSE;
        tell(p, STATE_PAUSE);
    }
    r0 = 0;
}

/* int sceAvPlayerResume(SceAvPlayerHandle handle) */
void vita_hle_sceAvPlayerResume(void) {
    player *p = by_handle(r0);
    if (p && p->state == STATE_PAUSE) {
        p->began += vita_clock_us() - p->paused;
        p->state = STATE_PLAY;
        tell(p, STATE_PLAY);
    }
    r0 = 0;
}

/* int sceAvPlayerSetLooping(SceAvPlayerHandle handle, SceBool looping) */
void vita_hle_sceAvPlayerSetLooping(void) {
    player *p = by_handle(r0);
    if (p) p->looping = r1 != 0;
    r0 = 0;
}

/* SceBool sceAvPlayerIsActive(SceAvPlayerHandle handle): playing or paused,
 * and not yet at the movie's end. */
void vita_hle_sceAvPlayerIsActive(void) {
    player *p = by_handle(r0);
    if (p) (void)keep_time(p);
    r0 = p && (p->state == STATE_PLAY || p->state == STATE_PAUSE);
}

/* uint64_t sceAvPlayerCurrentTime(SceAvPlayerHandle handle): milliseconds */
void vita_hle_sceAvPlayerCurrentTime(void) {
    player *p = by_handle(r0);
    const uint64_t ms = p ? keep_time(p) / 1000 : 0;
    r0 = (uint32_t)ms;
    r1 = (uint32_t)(ms >> 32);
}

/* --- streams ------------------------------------------------------------------------------ */

/* int sceAvPlayerStreamCount(SceAvPlayerHandle handle) */
void vita_hle_sceAvPlayerStreamCount(void) {
    player *p = by_handle(r0);
    r0 = p && p->film ? movie_streams(p->film) : 0;
}

/* int sceAvPlayerGetStreamInfo(SceAvPlayerHandle handle, uint32_t id, SceAvPlayerStreamInfo *info)
 *
 * The record: the stream's type (0 pictures, 1 sound, 2 timed text) and a
 * reserved word; sixteen bytes of details (a picture's width, height,
 * shape as a float and language; or sound's channels in sixteen bits, its
 * rate, the size of a block in bytes and its language); then the stream's
 * length and its start, in milliseconds. */
void vita_hle_sceAvPlayerGetStreamInfo(void) {
    player *p = by_handle(r0);
    const uint32_t info = r2;
    const movie_stream *s = p && p->film ? movie_stream_at(p->film, r1) : NULL;
    if (!s || !vita_mem_ptr(info, 0x28)) { r0 = SCE_AVPLAYER_ERROR_INVALID_PARAM; return; }
    for (uint32_t k = 0; k < 0x28; k += 4) vita_write32(info + k, 0);
    vita_write32(info, s->kind == MOVIE_VIDEO ? 0u : s->kind == MOVIE_AUDIO ? 1u : 2u);
    if (s->kind == MOVIE_VIDEO) {
        const float shape = s->height ? (float)s->width / (float)s->height : 0.0f;
        uint32_t bits;
        memcpy(&bits, &shape, 4);
        vita_write32(info + 0x08, s->width);
        vita_write32(info + 0x0C, s->height);
        vita_write32(info + 0x10, bits);
    } else if (s->kind == MOVIE_AUDIO) {
        vita_write16(info + 0x08, (uint16_t)s->channels);
        vita_write32(info + 0x0C, s->rate);
        vita_write32(info + 0x10, SOUND_FRAMES * s->channels * 2);
    }
    for (uint32_t k = 0; k < 4; k++) vita_write8(info + 0x14 + k, (uint8_t)s->language[k]);
    vita_write32(info + 0x18, (uint32_t)(s->duration_us / 1000));
    vita_write32(info + 0x1C, (uint32_t)(s->duration_us / 1000 >> 32));
    r0 = 0;
}

/* int sceAvPlayerEnableStream(SceAvPlayerHandle handle, uint32_t id), and the reverse */
static void enable(int on) {
    player *p = by_handle(r0);
    if (!p || !p->film || r1 >= movie_streams(p->film)) { r0 = SCE_AVPLAYER_ERROR_INVALID_PARAM; return; }
    if (on) p->enabled |= 1u << r1; else p->enabled &= ~(1u << r1);
    r0 = 0;
}
void vita_hle_sceAvPlayerEnableStream(void)  { enable(1); }
void vita_hle_sceAvPlayerDisableStream(void) { enable(0); }

/* --- what the program came for ------------------------------------------------------------- */

/* SceBool sceAvPlayerGetVideoData(SceAvPlayerHandle handle, SceAvPlayerFrameInfo *info)
 *
 * True when there is a picture the program has not had: its time has come.
 * The record is then where the picture is, a reserved word, its time in
 * milliseconds, and the details as sceAvPlayerGetStreamInfo gives them.
 * The picture stays where it is until the buffers have gone round. */
void vita_hle_sceAvPlayerGetVideoData(void) {
    player *p = by_handle(r0);
    const uint32_t info = r1;
    if (!p || !p->film || p->video < 0 || !((p->enabled >> p->video) & 1) || !vita_mem_ptr(info, 0x20)) { r0 = 0; return; }
    const uint64_t now = keep_time(p);
    if (p->state != STATE_PLAY) { r0 = 0; return; }

    /* The next picture is decoded when it is first asked after, and waits
     * here until the clock reaches its time. The one to hand out is the one
     * being shown now: a picture whose whole showing has gone by, because
     * the program was slow to ask, was still decoded, since those after it
     * are built on it, and is passed over. */
    for (;;) {
        if (!p->is_waiting) {
            const int got = p->video_over ? 0 : movie_next_video(p->film, p->waiting, &p->waiting_time, &p->waiting_shown);
            if (got < 0) vita_hle_trace("sceAvPlayerGetVideoData: a picture could not be decoded; the movie's pictures end here");
            if (got <= 0) { p->video_over = 1; r0 = 0; return; }
            p->is_waiting = 1;
        }
        if (p->waiting_time > now) { r0 = 0; return; }
        if (!p->waiting_shown || p->waiting_time + p->waiting_shown > now) break;
        p->is_waiting = 0;
        p->passed_over++;
    }

    const size_t bytes = movie_picture_bytes(p->width, p->height);
    const uint32_t at = p->picture[p->picture_next];
    void *out = vita_mem_ptr(at, (uint32_t)bytes);
    if (!out) { r0 = 0; return; }
    memcpy(out, p->waiting, bytes);
    p->picture_next = (p->picture_next + 1) % p->pictures;
    p->is_waiting = 0;
    p->handed_out++;

    const movie_stream *s = movie_stream_at(p->film, (uint32_t)p->video);
    const float shape = p->height ? (float)p->width / (float)p->height : 0.0f;
    const uint64_t ms = p->waiting_time / 1000;
    uint32_t bits;
    memcpy(&bits, &shape, 4);
    vita_write32(info + 0x00, at);
    vita_write32(info + 0x04, 0);
    vita_write32(info + 0x08, (uint32_t)ms);
    vita_write32(info + 0x0C, (uint32_t)(ms >> 32));
    vita_write32(info + 0x10, p->width);
    vita_write32(info + 0x14, p->height);
    vita_write32(info + 0x18, bits);
    for (uint32_t k = 0; k < 4; k++) vita_write8(info + 0x1C + k, (uint8_t)s->language[k]);
    r0 = 1;
}

/* SceBool sceAvPlayerGetAudioData(SceAvPlayerHandle handle, SceAvPlayerFrameInfo *info)
 *
 * The next block of the movie's sound, 1024 frames of sixteen bits with
 * the channels side by side, which a program hands to an audio port and so
 * takes no faster than it plays. It is given out a little before its time,
 * for the port to have it in hand, and no further ahead than that. */
void vita_hle_sceAvPlayerGetAudioData(void) {
    player *p = by_handle(r0);
    const uint32_t info = r1;
    if (!p || !p->film || p->audio < 0 || !((p->enabled >> p->audio) & 1) || p->audio_over || !vita_mem_ptr(info, 0x20)) { r0 = 0; return; }
    const uint64_t now = keep_time(p);
    if (p->state != STATE_PLAY) { r0 = 0; return; }

    const movie_stream *s = movie_stream_at(p->film, (uint32_t)p->audio);
    const uint32_t bytes = SOUND_FRAMES * s->channels * 2, at = p->sound[p->sound_next];
    int16_t *out = (int16_t *)vita_mem_ptr(at, bytes);
    uint64_t when = 0;
    if (!out || p->sound_time > now + SOUND_AHEAD) { r0 = 0; return; }
    memset(out, 0, bytes);
    const int got = movie_next_audio(p->film, out, SOUND_FRAMES, &when);
    if (got <= 0) { p->audio_over = 1; r0 = 0; return; }
    p->sound_time = when + (uint64_t)got * 1000000u / s->rate;
    p->sound_next = (p->sound_next + 1) % SOUND_BLOCKS;

    const uint64_t ms = when / 1000;
    vita_write32(info + 0x00, at);
    vita_write32(info + 0x04, 0);
    vita_write32(info + 0x08, (uint32_t)ms);
    vita_write32(info + 0x0C, (uint32_t)(ms >> 32));
    vita_write16(info + 0x10, (uint16_t)s->channels);
    vita_write16(info + 0x12, 0);
    vita_write32(info + 0x14, s->rate);
    vita_write32(info + 0x18, bytes);
    for (uint32_t k = 0; k < 4; k++) vita_write8(info + 0x1C + k, (uint8_t)s->language[k]);
    r0 = 1;
}
