/* hle_savedata.c — save data: the slots, the files, and the dialog about them.
 *
 * A title's saves are slots, each a record of what the system's own screens
 * show about it (a title, a subtitle, a date), and beside the records the
 * title's files. SceAppUtil keeps both: the title makes a slot, writes its
 * files through sceAppUtilSaveDataDataSave, and reads them back as ordinary
 * files under "savedata0:".
 *
 * Here the files go where the host mounted savedata0 (host.h), and a slot's
 * record is one more file there, sce_sys/slot_NNN.bin, the record as the
 * title gave it. With no such mount a run keeps its slots in memory and its
 * files nowhere: it can save, and the next run starts with nothing.
 *
 * The dialog is the system's screen for all this: "save here?", a list of
 * slots, "saving...", an error. The title drives it as a machine of two
 * levels. A dialog is RUNNING from its start until the title finishes it.
 * Inside that, each screen is RUNNING until it is answered, by the user
 * where it has buttons or a list, or closed by the title where it has
 * neither; then it is FINISHED, the title reads the result, and either
 * puts up the next screen (sceSaveDataDialogContinue) or ends the dialog
 * (sceSaveDataDialogFinish). There is no such screen here and nobody to
 * ask. A screen that wants an answer gets the affirmative one, OK or Yes
 * or the slot the list had focused, when it has been up for half a second
 * of the title's time: what a player pressing the confirm button through
 * it would give.
 *
 * Structure layouts are the SDK's (vita-headers for SceAppUtil; the dialog's
 * are not in it and are as the Vita3K project documents them).
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"
#include "clock.h"
#include "hle_vfs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#endif

static uint32_t arg(int n) { return vita_read32(sp + 4u * (uint32_t)(n - 4)); }   /* the nth argument, n >= 4 */

/* --- slots ------------------------------------------------------------------------------ */

#define SCE_APPUTIL_ERROR_PARAMETER                0x80100600u
#define SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS     0x80100640u
#define SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND  0x80100641u

#define SLOTS            256
#define SLOT_PARAM_SIZE  0x34Cu             /* SceAppUtilSaveDataSlotParam */
#define SLOT_PARAM_TIME  0x30Cu             /* its modifiedTime, a SceDateTime */

static struct { uint8_t known, exists; uint8_t param[SLOT_PARAM_SIZE]; } g_slot[SLOTS];

/* The host file behind a path under savedata0, with the directories above
 * it made if `make` is set. 0 when the host mounted no such thing. */
static int save_path(const char *under, int make, char *host, size_t cap) {
    char guest[600];
    if (strncmp(under, "savedata0:", 10) == 0) under += 10;     /* a title may name the mount itself */
    snprintf(guest, sizeof(guest), "savedata0:%s", under);
    if (!vita_io_resolve(guest, host, cap)) return 0;
    if (make) {
        /* every directory between the mount and the file, whether or not it is there already */
        char mount[1024];
        size_t root = vita_io_resolve("savedata0:", mount, sizeof(mount)) ? strlen(mount) : 0;
        while (root && (mount[root - 1] == '/' || mount[root - 1] == '\\')) mount[--root] = '\0';
        make_dir(mount);
        for (size_t i = root + 1; i < strlen(host); i++) {
            if (host[i] != '/' && host[i] != '\\') continue;
            char keep = host[i];
            host[i] = '\0';
            make_dir(host);
            host[i] = keep;
        }
    }
    return 1;
}

static void slot_file(uint32_t id, char *under, size_t cap) { snprintf(under, cap, "sce_sys/slot_%03u.bin", id); }

/* What is known of a slot, read from the mount the first time it is asked about. */
static int slot_exists(uint32_t id) {
    if (id >= SLOTS) return 0;
    if (!g_slot[id].known) {
        char under[64], host[1024];
        g_slot[id].known = 1;
        slot_file(id, under, sizeof(under));
        if (save_path(under, 0, host, sizeof(host))) {
            FILE *f = fopen(host, "rb");
            if (f) {
                g_slot[id].exists = fread(g_slot[id].param, 1, SLOT_PARAM_SIZE, f) == SLOT_PARAM_SIZE;
                fclose(f);
            }
        }
    }
    return g_slot[id].exists;
}

/* Keep a slot's record: the title's, stamped with the time it was kept. */
static void slot_keep(uint32_t id, uint32_t param) {
    char under[64], host[1024];
    uint8_t *rec = g_slot[id].param;
    const void *src = param ? vita_mem_ptr(param, SLOT_PARAM_SIZE) : NULL;
    if (src) memcpy(rec, src, SLOT_PARAM_SIZE);
    else if (!g_slot[id].exists) memset(rec, 0, SLOT_PARAM_SIZE);

    time_t now = (time_t)(vita_clock_unix_us() / 1000000ull);
    struct tm *t = localtime(&now);
    uint16_t stamp[6] = { (uint16_t)(t->tm_year + 1900), (uint16_t)(t->tm_mon + 1), (uint16_t)t->tm_mday,
                          (uint16_t)t->tm_hour, (uint16_t)t->tm_min, (uint16_t)t->tm_sec };
    for (int i = 0; i < 6; i++) { rec[SLOT_PARAM_TIME + 2 * i] = (uint8_t)stamp[i]; rec[SLOT_PARAM_TIME + 2 * i + 1] = (uint8_t)(stamp[i] >> 8); }
    memset(rec + SLOT_PARAM_TIME + 12, 0, 4);
    memset(rec, 0, 4);                                  /* status: available */

    g_slot[id].known = g_slot[id].exists = 1;
    slot_file(id, under, sizeof(under));
    if (save_path(under, 1, host, sizeof(host))) {
        FILE *f = fopen(host, "wb");
        if (f) { fwrite(rec, 1, SLOT_PARAM_SIZE, f); fclose(f); }
    }
}

/* int sceAppUtilSaveDataSlotCreate(unsigned int slotId, SceAppUtilSaveDataSlotParam *param, mountPoint) */
void vita_hle_sceAppUtilSaveDataSlotCreate(void) {
    uint32_t id = r0;
    if (id >= SLOTS) { r0 = SCE_APPUTIL_ERROR_PARAMETER; return; }
    if (slot_exists(id)) { r0 = SCE_APPUTIL_ERROR_SAVEDATA_SLOT_EXISTS; }
    else { slot_keep(id, r1); r0 = 0; }
    vita_hle_trace("sceAppUtilSaveDataSlotCreate(%u) -> 0x%X", id, r0);
}

/* int sceAppUtilSaveDataSlotDelete(unsigned int slotId, mountPoint) */
void vita_hle_sceAppUtilSaveDataSlotDelete(void) {
    uint32_t id = r0;
    char under[64], host[1024];
    if (!slot_exists(id)) { r0 = SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND; return; }
    g_slot[id].exists = 0;
    slot_file(id, under, sizeof(under));
    if (save_path(under, 0, host, sizeof(host))) remove(host);
    r0 = 0;
}

/* int sceAppUtilSaveDataSlotSetParam(unsigned int slotId, SceAppUtilSaveDataSlotParam *param, mountPoint) */
void vita_hle_sceAppUtilSaveDataSlotSetParam(void) {
    uint32_t id = r0;
    if (!slot_exists(id)) { r0 = SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND; return; }
    slot_keep(id, r1);
    r0 = 0;
}

/* int sceAppUtilSaveDataSlotGetParam(unsigned int slotId, SceAppUtilSaveDataSlotParam *param, mountPoint) */
void vita_hle_sceAppUtilSaveDataSlotGetParam(void) {
    uint32_t id = r0;
    void *dst = vita_mem_ptr(r1, SLOT_PARAM_SIZE);
    if (!slot_exists(id)) { vita_hle_trace("sceAppUtilSaveDataSlotGetParam(%u): no such slot", id); r0 = SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND; return; }
    if (dst) memcpy(dst, g_slot[id].param, SLOT_PARAM_SIZE);
    r0 = 0;
}

/* int sceAppUtilSaveDataDataSave(SceAppUtilSaveDataFileSlot *slot, SceAppUtilSaveDataFile *files,
 *                                unsigned int fileNum, mountPoint, SceSize *requiredSizeKB)
 *
 * The slot is { u32 id; SceAppUtilSaveDataSlotParam *slotParam; ... }, and a
 * file 0x40 bytes: { const char *filePath; void *buf; u32 bufSize; u32 pad;
 * s64 offset; u32 mode; u32 progDelta; ... }, mode 0 to write into a file
 * at the offset, 1 to cut it short first, 2 to make a directory. */
void vita_hle_sceAppUtilSaveDataDataSave(void) {
    uint32_t slot = r0, files = r1, count = r2, required = arg(4), kept = 0;
    static int said;
    if (vita_mem_ptr(required, 4)) vita_write32(required, 0);
    if (count > 4096 || (count && !vita_mem_ptr(files, count * 0x40u))) { r0 = SCE_APPUTIL_ERROR_PARAMETER; return; }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t item = files + i * 0x40u, path = vita_read32(item), buf = vita_read32(item + 4), size = vita_read32(item + 8);
        uint64_t offset = vita_read32(item + 16) | ((uint64_t)vita_read32(item + 20) << 32);
        uint32_t mode = vita_read32(item + 24);
        const char *name = vita_mem_avail(path) ? (const char *)vita_mem_ptr(path, 1) : NULL;
        char host[1024];
        if (!name || !memchr(name, 0, vita_mem_avail(path) < 256 ? vita_mem_avail(path) : 256)) continue;
        if (!save_path(name, 1, host, sizeof(host))) {
            if (!said) { said = 1; vita_hle_trace("sceAppUtilSaveDataDataSave: no savedata0 mount; what is saved is not kept"); }
            continue;
        }
        if (mode == 2) { make_dir(host); kept++; continue; }
        const void *src = size ? vita_mem_ptr(buf, size) : NULL;
        FILE *f = mode == 1 ? fopen(host, "wb") : fopen(host, "r+b");
        if (!f) f = fopen(host, "wb");
        if (!f) continue;
        if (src && fseek(f, (long)offset, SEEK_SET) == 0) fwrite(src, 1, size, f);
        fclose(f);
        kept++;
    }

    /* a slot named with the files is made or brought up to date with them */
    if (vita_mem_ptr(slot, 8)) {
        uint32_t id = vita_read32(slot), param = vita_read32(slot + 4);
        if (id < SLOTS && (param || !slot_exists(id))) slot_keep(id, param);
    }
    vita_hle_trace("sceAppUtilSaveDataDataSave: %u of %u files kept", kept, count);
    r0 = 0;
}

/* int sceAppUtilSaveDataDataRemove(SceAppUtilSaveDataFileSlot *slot, SceAppUtilSaveDataRemoveItem *files,
 *                                  unsigned int fileNum, mountPoint): an item is { const char *dataPath; int mode; ... }, 0x2C bytes */
void vita_hle_sceAppUtilSaveDataDataRemove(void) {
    uint32_t files = r1, count = r2;
    if (count > 4096 || (count && !vita_mem_ptr(files, count * 0x2Cu))) { r0 = SCE_APPUTIL_ERROR_PARAMETER; return; }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t path = vita_read32(files + i * 0x2Cu);
        const char *name = vita_mem_avail(path) ? (const char *)vita_mem_ptr(path, 1) : NULL;
        char host[1024];
        if (name && memchr(name, 0, vita_mem_avail(path) < 256 ? vita_mem_avail(path) : 256) && save_path(name, 0, host, sizeof(host)))
            remove(host);
    }
    r0 = 0;
}

/* int sceAppUtilSaveDataGetQuota(SceSize *quotaSizeKB, SceSize *usedSizeKB, mountPoint): a gigabyte, none of it used */
void vita_hle_sceAppUtilSaveDataGetQuota(void) {
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, 1024 * 1024);
    if (vita_mem_ptr(r1, 4)) vita_write32(r1, 0);
    r0 = 0;
}
void vita_hle_sceAppUtilSaveDataMount(void)  { r0 = 0; }
void vita_hle_sceAppUtilSaveDataUmount(void) { r0 = 0; }

/* --- the dialog ------------------------------------------------------------------------- */

#define SCE_COMMON_DIALOG_ERROR_BUSY          0x80020401u
#define SCE_COMMON_DIALOG_ERROR_NULL          0x80020402u
#define SCE_COMMON_DIALOG_ERROR_NOT_RUNNING   0x80020404u
#define SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED 0x80020405u
#define SCE_COMMON_DIALOG_ERROR_NOT_FINISHED  0x80020410u
#define SCE_COMMON_DIALOG_ERROR_NOT_IN_USE    0x80020411u

enum { STATUS_NONE = 0, STATUS_RUNNING = 1, STATUS_FINISHED = 2 };
enum { RESULT_OK = 0, RESULT_USER_CANCELED = 1, RESULT_ABORTED = 2 };
enum { MODE_FIXED = 1, MODE_LIST, MODE_USER_MSG, MODE_SYSTEM_MSG, MODE_ERROR_CODE, MODE_PROGRESS_BAR };
enum { TYPE_SAVE = 1, TYPE_LOAD, TYPE_DELETE };
enum { BUTTON_INVALID = 0, BUTTON_OK = 1 };             /* Yes is the same number as OK */

#define ANSWER_AFTER_US 500000u

static struct {
    int      in_use;
    uint32_t status, sub;
    uint32_t mode, type;
    int      asks;                          /* the screen up now waits for an answer */
    uint64_t answer_at;
    uint32_t result, button, slot, userdata;
} g_dialog;

/* Put a screen up: what it is, which slot it is about, whether it asks. */
static void screen(uint32_t p) {
    uint32_t mode = vita_read32(p + 0x50), type = vita_read32(p + 0x54);
    g_dialog.mode = mode;
    if (type) g_dialog.type = type;
    g_dialog.userdata = vita_read32(p + 0x78);
    g_dialog.status = g_dialog.sub = STATUS_RUNNING;
    g_dialog.result = RESULT_OK;
    g_dialog.button = BUTTON_INVALID;
    g_dialog.asks = 0;

    switch (mode) {
    case MODE_FIXED: {
        /* one slot and no question: it is answered as it goes up */
        uint32_t q = vita_read32(p + 0x58);
        if (vita_mem_ptr(q, 16)) g_dialog.slot = vita_read32(q);
        g_dialog.sub = STATUS_FINISHED;
        break;
    }
    case MODE_LIST: {
        /* { slotList, slotListSize, focusPos, focusId, listTitle, itemStyle }: the answer is the focused slot */
        uint32_t q = vita_read32(p + 0x5C);
        if (vita_mem_ptr(q, 24)) {
            uint32_t list = vita_read32(q), n = vita_read32(q + 4), focus = vita_read32(q + 8), chosen = 0xFFFFFFFFu;
            if (n > 256) n = 256;
            if (!vita_mem_ptr(list, n * 16u)) n = 0;
            if (focus == 8) chosen = vita_read32(q + 12);
            for (uint32_t i = 0; i < n && chosen == 0xFFFFFFFFu; i++) {
                uint32_t id = vita_read32(list + (focus == 1 || focus == 3 || focus == 5 ? n - 1 - i : i) * 16u);
                int want_empty = focus == 2 || focus == 3, want_data = focus >= 4 && focus <= 7;
                if ((want_empty && slot_exists(id)) || (want_data && !slot_exists(id))) continue;
                chosen = id;
            }
            if (chosen == 0xFFFFFFFFu && n) chosen = vita_read32(list);
            g_dialog.slot = chosen == 0xFFFFFFFFu ? 0 : chosen;
        }
        /* nothing can be loaded from, or deleted in, a slot with nothing in it */
        if (g_dialog.type != TYPE_SAVE && !slot_exists(g_dialog.slot)) g_dialog.result = RESULT_USER_CANCELED;
        g_dialog.asks = 1;
        break;
    }
    case MODE_USER_MSG: {
        /* { buttonType, msg, targetSlot }: OK, Yes and No, or no buttons at all */
        uint32_t q = vita_read32(p + 0x60);
        if (vita_mem_ptr(q, 24)) {
            g_dialog.slot = vita_read32(q + 8);
            g_dialog.asks = vita_read32(q) != 2;
        }
        if (g_dialog.asks) g_dialog.button = BUTTON_OK;
        break;
    }
    case MODE_SYSTEM_MSG: {
        /* { sysMsgType, value, targetSlot }: "no space" and "in progress" have no buttons */
        uint32_t q = vita_read32(p + 0x64);
        if (vita_mem_ptr(q, 24)) {
            uint32_t kind = vita_read32(q);
            g_dialog.slot = vita_read32(q + 8);
            g_dialog.asks = kind != 4 && kind != 5;
        }
        if (g_dialog.asks) g_dialog.button = BUTTON_OK;
        break;
    }
    case MODE_ERROR_CODE: {
        uint32_t q = vita_read32(p + 0x68);
        if (vita_mem_ptr(q, 20)) g_dialog.slot = vita_read32(q + 4);
        g_dialog.asks = 1;
        g_dialog.button = BUTTON_OK;
        break;
    }
    case MODE_PROGRESS_BAR: {
        /* { barType, sysMsgParam (0x38 bytes), msg, targetSlot }: the title closes it */
        uint32_t q = vita_read32(p + 0x6C);
        if (vita_mem_ptr(q, 0x50)) g_dialog.slot = vita_read32(q + 0x40);
        break;
    }
    default:
        break;
    }
    if (g_dialog.asks) g_dialog.answer_at = vita_clock_us() + ANSWER_AFTER_US;
    vita_hle_trace("save data dialog: mode %u, type %u, slot %u, %s", mode, g_dialog.type, g_dialog.slot,
                   g_dialog.sub == STATUS_FINISHED ? "answered as it goes up" :
                   g_dialog.asks ? "to be answered yes" : "up until the title closes it");
}

/* The answer, when the screen has been up long enough. */
static void answer(void) {
    if (g_dialog.in_use && g_dialog.sub == STATUS_RUNNING && g_dialog.asks && vita_clock_us() >= g_dialog.answer_at) {
        g_dialog.sub = STATUS_FINISHED;
        g_dialog.asks = 0;
    }
}

/* int sceSaveDataDialogInit(const SceSaveDataDialogParam *param) */
void vita_hle_sceSaveDataDialogInit(void) {
    if (!vita_mem_ptr(r0, 0x9C)) { r0 = SCE_COMMON_DIALOG_ERROR_NULL; return; }
    if (g_dialog.in_use) { r0 = SCE_COMMON_DIALOG_ERROR_BUSY; return; }
    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.in_use = 1;
    screen(r0);
    r0 = 0;
}

/* int sceSaveDataDialogContinue(const SceSaveDataDialogParam *param): the next screen of the same dialog */
void vita_hle_sceSaveDataDialogContinue(void) {
    if (!vita_mem_ptr(r0, 0x9C)) { r0 = SCE_COMMON_DIALOG_ERROR_NULL; return; }
    if (!g_dialog.in_use || g_dialog.status != STATUS_RUNNING) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_RUNNING; return; }
    answer();
    if (g_dialog.sub != STATUS_FINISHED) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_FINISHED; return; }
    screen(r0);
    r0 = 0;
}

/* int sceSaveDataDialogFinish(const SceSaveDataDialogFinishParam *param): the dialog is over */
void vita_hle_sceSaveDataDialogFinish(void) {
    if (!vita_mem_ptr(r0, 4)) { r0 = SCE_COMMON_DIALOG_ERROR_NULL; return; }
    if (!g_dialog.in_use || g_dialog.status != STATUS_RUNNING) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_RUNNING; return; }
    answer();
    if (g_dialog.sub != STATUS_FINISHED) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_FINISHED; return; }
    g_dialog.status = STATUS_FINISHED;
    g_dialog.sub = STATUS_RUNNING;
    vita_hle_trace("save data dialog: finished");
    r0 = 0;
}

/* int sceSaveDataDialogSubClose(void): the title takes down a screen that has no buttons */
void vita_hle_sceSaveDataDialogSubClose(void) {
    if (!g_dialog.in_use || g_dialog.sub != STATUS_RUNNING) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_RUNNING; return; }
    g_dialog.sub = STATUS_FINISHED;
    g_dialog.asks = 0;
    g_dialog.result = RESULT_OK;
    g_dialog.button = BUTTON_INVALID;
    r0 = 0;
}

/* int sceSaveDataDialogAbort(void) */
void vita_hle_sceSaveDataDialogAbort(void) {
    if (!g_dialog.in_use) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_IN_USE; return; }
    g_dialog.status = STATUS_FINISHED;
    g_dialog.sub = STATUS_RUNNING;
    g_dialog.result = RESULT_ABORTED;
    r0 = 0;
}

/* SceCommonDialogStatus sceSaveDataDialogGetStatus(void) */
void vita_hle_sceSaveDataDialogGetStatus(void) {
    answer();
    r0 = !g_dialog.in_use || (g_dialog.status != STATUS_RUNNING && g_dialog.sub != STATUS_RUNNING) ? STATUS_NONE : g_dialog.status;
}

/* SceCommonDialogStatus sceSaveDataDialogGetSubStatus(void) */
void vita_hle_sceSaveDataDialogGetSubStatus(void) {
    answer();
    r0 = !g_dialog.in_use || g_dialog.status != STATUS_RUNNING ? STATUS_NONE : g_dialog.sub;
}

/* int sceSaveDataDialogGetResult(SceSaveDataDialogResult *result)
 * { s32 mode, result, buttonId, slotId; SceSaveDataDialogSlotInfo *slotInfo; void *userdata; ... }
 * and the slot information, which the title supplies room for, is
 * { u32 isExist; SceAppUtilSaveDataSlotParam *slotParam; ... }. */
void vita_hle_sceSaveDataDialogGetResult(void) {
    uint32_t out = r0;
    if (!g_dialog.in_use) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_FINISHED; return; }
    if (!vita_mem_ptr(out, 0x18)) { r0 = SCE_COMMON_DIALOG_ERROR_NULL; return; }
    answer();
    if (g_dialog.status != STATUS_FINISHED && g_dialog.sub != STATUS_FINISHED) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_FINISHED; return; }
    uint32_t info = vita_read32(out + 16);
    vita_write32(out + 0, g_dialog.mode);
    vita_write32(out + 4, g_dialog.result);
    vita_write32(out + 8, g_dialog.button);
    vita_write32(out + 12, g_dialog.slot);
    vita_write32(out + 20, g_dialog.userdata);
    if (vita_mem_ptr(info, 8)) {
        int exists = slot_exists(g_dialog.slot);
        void *param = exists ? vita_mem_ptr(vita_read32(info + 4), SLOT_PARAM_SIZE) : NULL;
        vita_write32(info, (uint32_t)exists);
        if (param) memcpy(param, g_slot[g_dialog.slot].param, SLOT_PARAM_SIZE);
    }
    vita_hle_trace("save data dialog: result %u, button %u, slot %u", g_dialog.result, g_dialog.button, g_dialog.slot);
    r0 = 0;
}

/* int sceSaveDataDialogTerm(void) */
void vita_hle_sceSaveDataDialogTerm(void) {
    if (!g_dialog.in_use) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_IN_USE; return; }
    if (g_dialog.status != STATUS_FINISHED) { r0 = SCE_COMMON_DIALOG_ERROR_NOT_FINISHED; return; }
    g_dialog.in_use = 0;
    g_dialog.status = g_dialog.sub = STATUS_NONE;
    r0 = 0;
}

/* int sceSaveDataDialogProgressBarInc(target, delta) and ...SetValue(target, rate): a bar nobody sees */
void vita_hle_sceSaveDataDialogProgressBarInc(void) {
    r0 = !g_dialog.in_use ? SCE_COMMON_DIALOG_ERROR_NOT_RUNNING : g_dialog.mode != MODE_PROGRESS_BAR ? SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED : 0;
}
void vita_hle_sceSaveDataDialogProgressBarSetValue(void) { vita_hle_sceSaveDataDialogProgressBarInc(); }
