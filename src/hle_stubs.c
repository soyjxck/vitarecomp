/* hle_stubs.c — the platform services a title asks about and can do without.
 *
 * Touch, motion, camera, audio output, the audio synthesiser, the dialogs,
 * the network, trophies, the JPEG codecs, the live area, screenshots: what
 * each one answers here is the honest minimum for a program to carry on as
 * if the device were idle. No touch, no motion, no camera, no network, a
 * finished dialog, a synthesiser that accepts voices and plays nothing. The
 * audio output port is the one with a side effect: a write to it takes as
 * long as the samples would, which is what paces a title's audio thread.
 *
 * Each answer is the one Vita3K's own implementation or the SDK documents
 * for the idle case, where a value matters; where none does, zero. A call
 * whose answer would change what a title does next, and is not known, stays
 * a named trap rather than a guess: the video decoders, the codec engine.
 * Calling convention: see hle.h.
 */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_sysmem.h"
#include "sched.h"

#include <string.h>

static void zero(uint32_t addr, uint32_t size) {
    if (!vita_mem_ptr(addr, size)) return;
    for (uint32_t k = 0; k + 4 <= size; k += 4) vita_write32(addr + k, 0);
    for (uint32_t k = size & ~3u; k < size; k++) vita_write8(addr + k, 0);
}

static uint32_t arg(int n) { return vita_read32(sp + 4u * (uint32_t)(n - 4)); }

/* --- touch, motion, camera -------------------------------------------------------- */

/* SceMotion: a device lying still. SceMotionState is 0xF8 bytes. */
void vita_hle_sceMotionReset(void)            { r0 = 0; }
void vita_hle_sceMotionStartSampling(void)    { r0 = 0; }
void vita_hle_sceMotionStopSampling(void)     { r0 = 0; }
void vita_hle_sceMotionSetDeadband(void)      { r0 = 0; }
void vita_hle_sceMotionSetTiltCorrection(void){ r0 = 0; }
void vita_hle_sceMotionSetAngleThreshold(void){ r0 = 0; }
void vita_hle_sceMotionMagnetometerOn(void)   { r0 = 0; }
void vita_hle_sceMotionMagnetometerOff(void)  { r0 = 0; }
void vita_hle_sceMotionGetState(void) {
    zero(r0, 0xF8);
    if (vita_mem_ptr(r0, 0xF8)) {
        /* gravity: 1 g down the y axis; identity rotation */
        vita_write32(r0 + 8, 0xBF800000u);           /* acceleration.y = -1 */
        vita_write32(r0 + 0x34, 0x3F800000u);        /* deviceQuat.w = 1   */
        for (int i = 0; i < 4; i++) {
            vita_write32(r0 + 0x38 + 20u * (uint32_t)i, 0x3F800000u);   /* rotationMatrix diagonal */
            vita_write32(r0 + 0x78 + 20u * (uint32_t)i, 0x3F800000u);   /* nedMatrix diagonal      */
        }
    }
    r0 = 0;
}

/* SceCamera: there is none. */
#define SCE_CAMERA_ERROR_NOT_ACTIVE 0x802E0007u
void vita_hle_sceCameraOpen(void)  { r0 = SCE_CAMERA_ERROR_NOT_ACTIVE; }
void vita_hle_sceCameraClose(void) { r0 = 0; }
void vita_hle_sceCameraStart(void) { r0 = SCE_CAMERA_ERROR_NOT_ACTIVE; }
void vita_hle_sceCameraStop(void)  { r0 = 0; }
void vita_hle_sceCameraRead(void)  { r0 = SCE_CAMERA_ERROR_NOT_ACTIVE; }
void vita_hle_sceCameraIsActive(void) { r0 = 0; }
void vita_hle_sceCameraSetAutoControlHold(void) { r0 = 0; }
void vita_hle_sceCameraSetWhiteBalance(void)    { r0 = 0; }
void vita_hle_sceCameraSetEV(void)              { r0 = 0; }

/* --- audio output --------------------------------------------------------------------
 * A port remembers its length and rate; a write to it takes the time the
 * samples would take to play, which paces the thread that feeds it, and
 * lets the other threads run meanwhile. */

typedef struct { int used; uint32_t len, freq, mode; } audio_port;
static audio_port g_ports[8];

/* int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) */
void vita_hle_sceAudioOutOpenPort(void) {
    int i;
    for (i = 0; i < 8 && g_ports[i].used; i++) {}
    if (i == 8) { r0 = 0x80260002u; return; }   /* SCE_AUDIO_OUT_ERROR_INVALID_PORT */
    g_ports[i].used = 1; g_ports[i].len = r1; g_ports[i].freq = r2 ? r2 : 48000; g_ports[i].mode = r3;
    vita_hle_trace("sceAudioOutOpenPort(type %u, len %u, %u Hz, mode %u) -> %d", r0, r1, r2, r3, i + 1);
    r0 = (uint32_t)i + 1;
}

/* int sceAudioOutOutput(int port, const void *buf): blocks for the buffer's duration */
void vita_hle_sceAudioOutOutput(void) {
    int i = (int)r0 - 1;
    if (i < 0 || i >= 8 || !g_ports[i].used) { r0 = 0x80260002u; return; }
    uint32_t us = (uint32_t)((uint64_t)g_ports[i].len * 1000000ull / g_ports[i].freq);
    vita_sched_delay(us ? us : 1000);
    r0 = 0;
}
void vita_hle_sceAudioOutReleasePort(void) { int i = (int)r0 - 1; if (i >= 0 && i < 8) g_ports[i].used = 0; r0 = 0; }
void vita_hle_sceAudioOutSetVolume(void)   { r0 = 0; }
void vita_hle_sceAudioOutSetConfig(void)   { int i = (int)r0 - 1; if (i >= 0 && i < 8 && g_ports[i].used) { if ((int)r1 > 0) g_ports[i].len = r1; if ((int)r2 > 0) g_ports[i].freq = r2; } r0 = 0; }
void vita_hle_sceAudioOutGetRestSample(void) { r0 = 0; }
void vita_hle_sceAudioOutGetAdopt(void)    { r0 = 1; }

/* The audio synthesiser, SceNgs, is in hle_ngs.c. */

/* A small record in guest memory for a stub that has to return an address. */
static uint32_t handle(void) {
    static uint32_t pool, left;
    if (left < 64) { pool = vita_sysmem_reserve(0x10000); left = pool ? 0x10000 : 0; }
    if (!pool) return 0;
    uint32_t h = pool; pool += 64; left -= 64;
    return h;
}

/* --- application services ------------------------------------------------------------- */

#define SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND 0x80100641u
#define SCE_APPMGR_ERROR_NO_EVENT                 0x80802004u

/* int sceAppUtilSystemParamGetInt(unsigned int paramId, int *value): English, cross is enter */
void vita_hle_sceAppUtilSystemParamGetInt(void) {
    uint32_t id = r0, v = 0;
    if (id == 1) v = 1;           /* SCE_SYSTEM_PARAM_ID_LANG: English (US) */
    if (id == 2) v = 1;           /* SCE_SYSTEM_PARAM_ID_ENTER_BUTTON: cross */
    if (vita_mem_ptr(r1, 4)) vita_write32(r1, v);
    r0 = 0;
}
void vita_hle_sceAppUtilSystemParamGetString(void) { if (vita_mem_ptr(r1, 2)) vita_write16(r1, 0); r0 = 0; }

/* Save data slots: none exist yet, creating one succeeds, saving succeeds. */
void vita_hle_sceAppUtilSaveDataSlotCreate(void)   { vita_hle_trace("sceAppUtilSaveDataSlotCreate(%u)", r0); r0 = 0; }
void vita_hle_sceAppUtilSaveDataSlotDelete(void)   { r0 = 0; }
void vita_hle_sceAppUtilSaveDataSlotSetParam(void) { r0 = 0; }
void vita_hle_sceAppUtilSaveDataSlotGetParam(void) { vita_hle_trace("sceAppUtilSaveDataSlotGetParam(%u): none", r0); r0 = SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND; }
void vita_hle_sceAppUtilSaveDataDataSave(void)     { if (vita_mem_ptr(arg(4), 4)) vita_write32(arg(4), 0); r0 = 0; }
void vita_hle_sceAppUtilSaveDataDataRemove(void)   { r0 = 0; }
void vita_hle_sceAppUtilSaveDataGetQuota(void)     { if (vita_mem_ptr(r0, 4)) vita_write32(r0, 1024 * 1024); if (vita_mem_ptr(r1, 4)) vita_write32(r1, 0); r0 = 0; }
void vita_hle_sceAppUtilSaveDataMount(void)        { r0 = 0; }
void vita_hle_sceAppUtilSaveDataUmount(void)       { r0 = 0; }

/* SceAppMgr: no system events are pending; the app is running in the foreground. */
void vita_hle_sceAppMgrReceiveSystemEvent(void) { r0 = SCE_APPMGR_ERROR_NO_EVENT; }
void vita_hle__sceAppMgrGetAppState(void)       { zero(r0, r1); r0 = 0; }
void vita_hle_sceAppMgrAcquireBgmPort(void)     { r0 = 0; }
void vita_hle_sceAppMgrReleaseBgmPort(void)     { r0 = 0; }

/* The common dialogs: every one finishes at once as cancelled. */
#define SCE_COMMON_DIALOG_STATUS_FINISHED 2
#define SCE_COMMON_DIALOG_RESULT_USER_CANCELED 1
void vita_hle_sceCommonDialogSetConfigParam(void) { r0 = 0; }
void vita_hle_sceCommonDialogUpdate(void)         { r0 = 0; }
void vita_hle_sceMsgDialogInit(void)      { vita_hle_trace("sceMsgDialogInit: finished at once"); r0 = 0; }
void vita_hle_sceMsgDialogGetStatus(void) { r0 = SCE_COMMON_DIALOG_STATUS_FINISHED; }
void vita_hle_sceMsgDialogGetResult(void) { zero(r0, 16); if (vita_mem_ptr(r0, 16)) { vita_write32(r0 + 4, SCE_COMMON_DIALOG_RESULT_USER_CANCELED); } r0 = 0; }
void vita_hle_sceMsgDialogTerm(void)      { r0 = 0; }
void vita_hle_sceMsgDialogClose(void)     { r0 = 0; }
void vita_hle_sceMsgDialogAbort(void)     { r0 = 0; }
void vita_hle_sceImeDialogInit(void)      { r0 = 0; }
void vita_hle_sceImeDialogGetStatus(void) { r0 = SCE_COMMON_DIALOG_STATUS_FINISHED; }
void vita_hle_sceImeDialogGetResult(void) { zero(r0, 16); if (vita_mem_ptr(r0, 16)) vita_write32(r0 + 4, SCE_COMMON_DIALOG_RESULT_USER_CANCELED); r0 = 0; }
void vita_hle_sceImeDialogTerm(void)      { r0 = 0; }
void vita_hle_sceImeDialogAbort(void)     { r0 = 0; }
void vita_hle_sceSaveDataDialogInit(void)      { r0 = 0; }
void vita_hle_sceSaveDataDialogGetStatus(void) { r0 = SCE_COMMON_DIALOG_STATUS_FINISHED; }
void vita_hle_sceSaveDataDialogGetSubStatus(void) { r0 = SCE_COMMON_DIALOG_STATUS_FINISHED; }
void vita_hle_sceSaveDataDialogGetResult(void) { zero(r0, 0x40); if (vita_mem_ptr(r0, 0x40)) vita_write32(r0 + 4, SCE_COMMON_DIALOG_RESULT_USER_CANCELED); r0 = 0; }
void vita_hle_sceSaveDataDialogContinue(void)  { r0 = 0; }
void vita_hle_sceSaveDataDialogFinish(void)    { r0 = 0; }
void vita_hle_sceSaveDataDialogSubClose(void)  { r0 = 0; }
void vita_hle_sceSaveDataDialogTerm(void)      { r0 = 0; }
void vita_hle_sceSaveDataDialogAbort(void)     { r0 = 0; }
void vita_hle_sceNetCheckDialogInit(void)      { r0 = 0; }
void vita_hle_sceNetCheckDialogGetStatus(void) { r0 = SCE_COMMON_DIALOG_STATUS_FINISHED; }
void vita_hle_sceNetCheckDialogGetResult(void) { zero(r0, 16); if (vita_mem_ptr(r0, 16)) vita_write32(r0 + 4, SCE_COMMON_DIALOG_RESULT_USER_CANCELED); r0 = 0; }
void vita_hle_sceNetCheckDialogTerm(void)      { r0 = 0; }
void vita_hle_sceNetCheckDialogAbort(void)     { r0 = 0; }
void vita_hle_sceNpTrophySetupDialogInit(void)      { r0 = 0; }
void vita_hle_sceNpTrophySetupDialogGetStatus(void) { r0 = SCE_COMMON_DIALOG_STATUS_FINISHED; }
void vita_hle_sceNpTrophySetupDialogGetResult(void) { zero(r0, 16); r0 = 0; }
void vita_hle_sceNpTrophySetupDialogTerm(void)      { r0 = 0; }
void vita_hle_sceNpTrophySetupDialogAbort(void)     { r0 = 0; }

/* The live area and screenshots: nothing to show. */
void vita_hle_sceLiveAreaReplaceAllSync(void)   { r0 = 0; }
void vita_hle_sceLiveAreaGetStatus(void)        { r0 = 0; }
void vita_hle_sceLiveAreaUpdateFrameSync(void)  { r0 = 0; }
void vita_hle_sceLiveAreaGetFrameRevision(void) { r0 = 0; }
void vita_hle_sceScreenShotSetParam(void)        { r0 = 0; }
void vita_hle_sceScreenShotSetOverlayImage(void) { r0 = 0; }
void vita_hle_sceScreenShotEnable(void)          { r0 = 0; }
void vita_hle_sceScreenShotDisable(void)         { r0 = 0; }

/* --- the network: disconnected -------------------------------------------------------- */

#define SCE_NET_ERROR_ENOTCONN  0x80410139u
#define SCE_NET_ERROR_EINVAL    0x80410116u
#define SCE_NP_ERROR_NOT_INITIALIZED 0x80550002u

void vita_hle_sceNetInit(void) { r0 = 0; }
void vita_hle_sceNetTerm(void) { r0 = 0; }
void vita_hle_sceNetSocket(void)  { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetSocketClose(void) { r0 = 0; }
void vita_hle_sceNetConnect(void) { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceNetBind(void)    { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetListen(void)  { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetAccept(void)  { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetSend(void)    { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceNetRecv(void)    { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceNetSetsockopt(void) { r0 = 0; }
void vita_hle_sceNetHtons(void)   { r0 = ((r0 & 0xFF) << 8) | ((r0 >> 8) & 0xFF); }
void vita_hle_sceNetNtohs(void)   { r0 = ((r0 & 0xFF) << 8) | ((r0 >> 8) & 0xFF); }
void vita_hle_sceNetHtonl(void)   { r0 = ((r0 & 0xFF) << 24) | ((r0 & 0xFF00) << 8) | ((r0 >> 8) & 0xFF00) | (r0 >> 24); }
void vita_hle_sceNetNtohl(void)   { vita_hle_sceNetHtonl(); }
void vita_hle_sceNetInetPton(void){ r0 = 0; }
void vita_hle_sceNetErrnoLoc(void){ static uint32_t slot; if (!slot) slot = handle(); r0 = slot; }
void vita_hle_sceNetResolverCreate(void)    { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetResolverDestroy(void)   { r0 = 0; }
void vita_hle_sceNetResolverStartNtoa(void) { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceNetEpollCreate(void)  { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetEpollControl(void) { r0 = SCE_NET_ERROR_EINVAL; }
void vita_hle_sceNetEpollWait(void)    { r0 = 0; }
void vita_hle_sceNetEpollDestroy(void) { r0 = 0; }
void vita_hle_sceNetCtlInit(void) { r0 = 0; }
void vita_hle_sceNetCtlTerm(void) { r0 = 0; }
void vita_hle_sceNetCtlInetGetState(void)  { if (vita_mem_ptr(r0, 4)) vita_write32(r0, 0); r0 = 0; }   /* SCE_NETCTL_STATE_DISCONNECTED */
void vita_hle_sceNetCtlInetGetInfo(void)   { zero(r1, 0x100); r0 = 0; }
void vita_hle_sceNetCtlInetGetResult(void) { if (vita_mem_ptr(r1, 4)) vita_write32(r1, SCE_NET_ERROR_ENOTCONN); r0 = 0; }
void vita_hle_sceNetCtlInetRegisterCallback(void)    { r0 = 0; }
void vita_hle_sceNetCtlInetUnregisterCallback(void)  { r0 = 0; }
void vita_hle_sceNetCtlAdhocRegisterCallback(void)   { r0 = 0; }
void vita_hle_sceNetCtlAdhocUnregisterCallback(void) { r0 = 0; }
void vita_hle_sceNetCtlCheckCallback(void) { r0 = 0; }
void vita_hle_sceHttpInit(void) { r0 = 0; }
void vita_hle_sceHttpTerm(void) { r0 = 0; }
void vita_hle_sceHttpCreateTemplate(void)          { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpDeleteTemplate(void)          { r0 = 0; }
void vita_hle_sceHttpCreateConnection(void)        { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpCreateConnectionWithURL(void) { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpDeleteConnection(void)        { r0 = 0; }
void vita_hle_sceHttpCreateRequestWithURL(void)    { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpDeleteRequest(void)           { r0 = 0; }
void vita_hle_sceHttpAddRequestHeader(void)        { r0 = 0; }
void vita_hle_sceHttpSendRequest(void)             { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpAbortRequest(void)            { r0 = 0; }
void vita_hle_sceHttpGetStatusCode(void)           { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpGetAllResponseHeaders(void)   { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceHttpReadData(void)                { r0 = SCE_NET_ERROR_ENOTCONN; }
void vita_hle_sceSslInit(void) { r0 = 0; }
void vita_hle_sceSslTerm(void) { r0 = 0; }
void vita_hle_sceNpInit(void) { r0 = 0; }
void vita_hle_sceNpTerm(void) { r0 = 0; }
void vita_hle_sceNpCheckCallback(void) { r0 = 0; }
void vita_hle_sceNpRegisterServiceStateCallback(void)   { r0 = 0; }
void vita_hle_sceNpUnregisterServiceStateCallback(void) { r0 = 0; }
void vita_hle_sceNpAuthInit(void) { r0 = 0; }
void vita_hle_sceNpAuthTerm(void) { r0 = 0; }
void vita_hle_sceNpAuthCreateStartRequest(void) { r0 = SCE_NP_ERROR_NOT_INITIALIZED; }
void vita_hle_sceNpAuthGetTicket(void)      { r0 = SCE_NP_ERROR_NOT_INITIALIZED; }
void vita_hle_sceNpAuthGetTicketParam(void) { r0 = SCE_NP_ERROR_NOT_INITIALIZED; }
void vita_hle_sceNpActivityInit(void) { r0 = 0; }
void vita_hle_sceNpActivityTerm(void) { r0 = 0; }
void vita_hle_sceNpActivityPostStatus(void) { r0 = 0; }
void vita_hle_sceNpTrophyInit(void) { r0 = 0; }
void vita_hle_sceNpTrophyTerm(void) { r0 = 0; }
void vita_hle_sceNpTrophyCreateContext(void) { if (vita_mem_ptr(r0, 4)) vita_write32(r0, 1); r0 = 0; }
void vita_hle_sceNpTrophyDestroyContext(void){ r0 = 0; }
void vita_hle_sceNpTrophyCreateHandle(void)  { if (vita_mem_ptr(r0, 4)) vita_write32(r0, 1); r0 = 0; }
void vita_hle_sceNpTrophyDestroyHandle(void) { r0 = 0; }
void vita_hle_sceNpTrophyUnlockTrophy(void)  { vita_hle_trace("sceNpTrophyUnlockTrophy(%d)", (int)r2); if (vita_mem_ptr(r3, 4)) vita_write32(r3, r2); r0 = 0; }
void vita_hle_sceNpTrophyGetTrophyUnlockState(void) { r0 = 0; }

/* --- the JPEG encoder: a context that encodes nothing worth looking at --------------- */

/* int sceJpegEncoderGetContextSize(void) */
void vita_hle_sceJpegEncoderGetContextSize(void)  { r0 = 0x2000; }
void vita_hle_sceJpegEncoderInit(void)            { r0 = 0; }
void vita_hle_sceJpegEncoderInitWithParam(void)   { r0 = 0; }
void vita_hle_sceJpegEncoderEnd(void)             { r0 = 0; }
void vita_hle_sceJpegEncoderSetValidRegion(void)  { r0 = 0; }
void vita_hle_sceJpegEncoderSetCompressionRatio(void) { r0 = 0; }
void vita_hle_sceJpegEncoderSetOutputAddr(void)   { r0 = 0; }
void vita_hle_sceJpegEncoderSetHeaderMode(void)   { r0 = 0; }
void vita_hle_sceJpegEncoderCsc(void)             { r0 = 0; }
/* int sceJpegEncoderEncode(context, const void *inBuffer): the bytes written; a
 * valid, empty JPEG would be better than a count with nothing behind it,
 * and comes with the renderer. */
void vita_hle_sceJpegEncoderEncode(void)          { vita_hle_trace("sceJpegEncoderEncode: no image"); r0 = 0x800; }

/* --- the C++ runtime's remaining entry points ------------------------------------------ */

void vita_hle__ZNSt9exceptionC2Ev(void)         { }
void vita_hle__ZNSt9exceptionD2Ev(void)         { }
void vita_hle__ZNSt8bad_castD1Ev(void)          { }
void vita_hle__ZNSt10bad_typeidD1Ev(void)       { }
void vita_hle__ZNSt12out_of_rangeD1Ev(void)     { }
void vita_hle__ZNSt12length_errorD1Ev(void)     { }
void vita_hle__ZNKSt9exception6_RaiseEv(void)   { vita_trap_unimpl(r0, 0, "std::exception::_Raise: exceptions are not unwound"); vita_host_exit(1); }
void vita_hle__ZNKSt12_String_base5_XranEv(void){ vita_trap_unimpl(r0, 0, "std::string: out of range, not unwound"); vita_host_exit(1); }
void vita_hle__ZNKSt12_String_base5_XlenEv(void){ vita_trap_unimpl(r0, 0, "std::string: length error, not unwound"); vita_host_exit(1); }

/* --- the JPEG decoder: no decoder, so every image is unsupported ---------------------- */

#define SCE_JPEG_ERROR_UNSUPPORT_IMAGE 0x80650003u
void vita_hle_sceJpegInitMJpeg(void)          { r0 = 0; }
void vita_hle_sceJpegInitMJpegWithParam(void) { r0 = 0; }
void vita_hle_sceJpegFinishMJpeg(void)        { r0 = 0; }
void vita_hle_sceJpegGetOutputInfo(void)      { r0 = SCE_JPEG_ERROR_UNSUPPORT_IMAGE; }
void vita_hle_sceJpegDecodeMJpeg(void)        { vita_hle_trace("sceJpegDecodeMJpeg: no decoder"); r0 = SCE_JPEG_ERROR_UNSUPPORT_IMAGE; }
void vita_hle_sceJpegDecodeMJpegYCbCr(void)   { vita_hle_trace("sceJpegDecodeMJpegYCbCr: no decoder"); r0 = SCE_JPEG_ERROR_UNSUPPORT_IMAGE; }
void vita_hle_sceJpegMJpegCsc(void)           { r0 = 0; }
