/*
 * SonAdvance-nx: native Nintendo Switch platform layer for the SAT-R
 * Sonic Advance decomp.
 *
 * Game code runs natively on the game thread. Each pass of the game's main
 * loop ends in VBlankIntrWait(), where we compose the GBA picture with the
 * software compositor, hand it to deko3d, and wait for the next display
 * refresh (the swapchain acquire blocks on vsync). Audio goes through an
 * SDL callback fed from a ring buffer. The 64 KiB flash save lives in RAM
 * and is written to the SD card at the end of any frame where it changed.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>
#include "romfill.h"
#include <deko3d.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "global.h"
#include "core.h"
#include "lib/agb_flash/flash_internal.h"
#include "platform/shared/dma.h"
#include "platform/shared/video/gpsp_renderer.h"
#include "game/shared/stage/stage.h"
#include "game/shared/stage/camera.h"
#include "sonadv_port.h"

#if ENABLE_AUDIO
#include "platform/shared/audio/cgb_audio.h"
#endif

#include "present_vsh_dksh.h"
#include "present_fsh_dksh.h"

#ifndef SONADV_BUILD_ID
#define SONADV_BUILD_ID "unknown build"
#endif
#ifndef SONADV_VERSION
#define SONADV_VERSION "0.0.0"
#endif

#define APP_DIR   "sdmc:/switch/SonAdvance-nx"
#define LOG_DIR   APP_DIR "/logs"
#define SAVE_PATH APP_DIR "/sa1.sav"
#define SAVE_TMP  APP_DIR "/sa1.sav.tmp"
#define SETTINGS_PATH APP_DIR "/settings.ini"
#define SETTINGS_TMP  APP_DIR "/settings.ini.tmp"

#define SAVE_FILE_SIZE (64 * 1024)

extern void AgbMain(void);

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

static FILE *sLog;
static bool sLogsOk;

static void LogF(const char *fmt, ...)
{
    if (sLog == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sLog, fmt, ap);
    va_end(ap);
    fflush(sLog);
}

static void RotateLog(const char *cur, const char *prev)
{
    remove(prev);
    if (rename(cur, prev) != 0)
        remove(cur); /* never append to a stale log */
}

static void OpenLogs(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(LOG_DIR, 0777);

    RotateLog(LOG_DIR "/sonadv.log", LOG_DIR "/sonadv.prev.log");
    RotateLog(LOG_DIR "/sonadv.err.log", LOG_DIR "/sonadv.prev.err.log");

    sLog = fopen(LOG_DIR "/sonadv.log", "a");
    if (sLog == NULL)
        sLog = fopen(APP_DIR "/sonadv.log", "a");
    if (sLog == NULL)
        sLog = fopen("sdmc:/sonadv.log", "a");
    if (sLog != NULL)
        setvbuf(sLog, NULL, _IOFBF, 16 * 1024);

    /* stderr gets its own file: Horizon refuses a second write handle. */
    FILE *err = freopen(LOG_DIR "/sonadv.err.log", "a", stderr);
    sLogsOk = (sLog != NULL) && (err != NULL);

    LogF("SonAdvance-nx %s, %s\n", SONADV_VERSION, SONADV_BUILD_ID);
    LogF("logs: %s\n", sLogsOk ? "OK" : "PARTIAL (one stream could not open)");
}

static void LogClocks(const char *why)
{
    u32 cpu = 0, gpu = 0, mem = 0;
    ClkrstSession s;
    if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3))) {
        clkrstGetClockRate(&s, &cpu);
        clkrstCloseSession(&s);
    }
    if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_GPU, 3))) {
        clkrstGetClockRate(&s, &gpu);
        clkrstCloseSession(&s);
    }
    if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_EMC, 3))) {
        clkrstGetClockRate(&s, &mem);
        clkrstCloseSession(&s);
    }
    LogF("clocks (%s): cpu %u MHz, gpu %u MHz, mem %u MHz\n", why, cpu / 1000000, gpu / 1000000, mem / 1000000);
}

/* ------------------------------------------------------------------------ */
/* Crash handler: writes faulting context as offsets from the module start  */
/* ------------------------------------------------------------------------ */

/* Exit through AM (ISelfController::Exit) the way mp2-nx does: the mode is
 * set at exit time, and __libnx_exit below ends the process itself if the
 * applet teardown did not take over the exit path. */
u32 __nx_applet_exit_mode = 0;
static LoaderReturnFn sLoaderReturn;

extern void __appExit(void);
extern void NX_NORETURN __nx_exit(Result rc, LoaderReturnFn retaddr);

void NX_NORETURN __libnx_exit(int rc)
{
    (void)rc;
    __appExit();
    if (envGetExitFuncPtr() == sLoaderReturn)
        svcExitProcess();
    __nx_exit(0, envGetExitFuncPtr());
}

alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static u64 sModuleBase;
static char sCrashBuf[4096];

static u64 FindModuleBase(void)
{
    MemoryInfo info;
    u32 page;
    if (R_FAILED(svcQueryMemory(&info, &page, (u64)&FindModuleBase)))
        return 0;
    return info.addr;
}

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    int n = snprintf(sCrashBuf, sizeof(sCrashBuf),
                     "CRASH in %s\nerror_desc 0x%x\npc   +0x%llx\nlr   +0x%llx\nsp   0x%llx\nfar  0x%llx\nmodule base 0x%llx\n",
                     SONADV_BUILD_ID, ctx->error_desc, (unsigned long long)(ctx->pc.x - sModuleBase),
                     (unsigned long long)(ctx->lr.x - sModuleBase), (unsigned long long)ctx->sp.x,
                     (unsigned long long)ctx->far.x, (unsigned long long)sModuleBase);
    for (int i = 0; i < 29 && n < (int)sizeof(sCrashBuf) - 64; i++) {
        u64 v = ctx->cpu_gprs[i].x;
        if (v >= sModuleBase && v < sModuleBase + 0x10000000)
            n += snprintf(sCrashBuf + n, sizeof(sCrashBuf) - n, "x%-2d  +0x%llx\n", i, (unsigned long long)(v - sModuleBase));
        else
            n += snprintf(sCrashBuf + n, sizeof(sCrashBuf) - n, "x%-2d  0x%llx\n", i, (unsigned long long)v);
    }

    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    FsFileSystem ownFs;
    if (fs == NULL && R_SUCCEEDED(fsOpenSdCardFileSystem(&ownFs)))
        fs = &ownFs; /* sdmc already unmounted (crash during exit) */
    if (fs != NULL) {
        char path[FS_MAX_PATH];
        snprintf(path, sizeof(path), "/switch/SonAdvance-nx/logs/crash_%llx.txt", (unsigned long long)armGetSystemTick());
        fsFsCreateFile(fs, path, 0, 0);
        FsFile f;
        if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &f))) {
            fsFileWrite(&f, 0, sCrashBuf, n, FsWriteOption_Flush);
            fsFileClose(&f);
        }
    }
    svcExitProcess();
}

/* ------------------------------------------------------------------------ */
/* Save data                                                                */
/* ------------------------------------------------------------------------ */

static u8 sSaveShadow[SAVE_FILE_SIZE];
static u32 sSaveWrites;

static void ReadSaveFile(void)
{
    memset(FLASH_BASE, 0xFF, SAVE_FILE_SIZE);
    FILE *f = fopen(SAVE_PATH, "rb");
    if (f != NULL) {
        size_t got = fread(FLASH_BASE, 1, SAVE_FILE_SIZE, f);
        fclose(f);
        LogF("save: loaded %u bytes from %s\n", (unsigned)got, SAVE_PATH);
    } else {
        LogF("save: none yet (%s)\n", SAVE_PATH);
    }
    memcpy(sSaveShadow, FLASH_BASE, SAVE_FILE_SIZE);
}

/* Called at the end of every frame: write only when the game changed it.
 * Write to a temp file, then delete and rename (FAT refuses rename onto an
 * existing file). */
static void FlushSaveIfChanged(bool force)
{
    if (!force && memcmp(sSaveShadow, FLASH_BASE, SAVE_FILE_SIZE) == 0)
        return;

    FILE *f = fopen(SAVE_TMP, "wb");
    if (f == NULL) {
        LogF("save: cannot open %s (errno %d)\n", SAVE_TMP, errno);
        return;
    }
    size_t put = fwrite(FLASH_BASE, 1, SAVE_FILE_SIZE, f);
    fclose(f);
    if (put != SAVE_FILE_SIZE) {
        LogF("save: short write %u\n", (unsigned)put);
        return;
    }
    remove(SAVE_PATH);
    if (rename(SAVE_TMP, SAVE_PATH) != 0) {
        LogF("save: rename failed (errno %d)\n", errno);
        return;
    }
    memcpy(sSaveShadow, FLASH_BASE, SAVE_FILE_SIZE);
    sSaveWrites++;
    LogF("save: written to SD (%u)\n", sSaveWrites);
}

/* Kept for code that asks for an immediate flush (SA2's save code). */
void Platform_StoreSaveFile(void) { FlushSaveIfChanged(false); }

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

static PadState sPad;
static u64 sButtonsHeld, sButtonsDown;
static bool sHoldStartSelect; /* after the port menu: keep + and - from the game until released */

/* Remappable GBA buttons. Default: Nintendo Switch Online GBA layout (A is A,
 * B is B, Plus is Start, Minus is Select). The D-pad and left stick are fixed. */
enum { MAP_A, MAP_B, MAP_L, MAP_R, MAP_START, MAP_SELECT, MAP_COUNT };
static const char *const sMapNames[MAP_COUNT] = { "A", "B", "L", "R", "Start", "Select" };
static const char *const sMapKeys[MAP_COUNT] = { "map_a", "map_b", "map_l", "map_r", "map_start", "map_select" };
static const u16 sMapGba[MAP_COUNT] = { A_BUTTON, B_BUTTON, L_BUTTON, R_BUTTON, START_BUTTON, SELECT_BUTTON };
static const u64 sMapDefault[MAP_COUNT] = { HidNpadButton_A, HidNpadButton_B, HidNpadButton_L, HidNpadButton_R,
                                            HidNpadButton_Plus, HidNpadButton_Minus };
static u64 sMap[MAP_COUNT] = { HidNpadButton_A, HidNpadButton_B, HidNpadButton_L, HidNpadButton_R,
                               HidNpadButton_Plus, HidNpadButton_Minus };

/* Switch buttons a GBA button can be assigned to, with their names. */
static const struct {
    u64 bit;
    const char *name;
} sSwitchButtons[] = {
    { HidNpadButton_A, "A" },   { HidNpadButton_B, "B" },   { HidNpadButton_X, "X" },       { HidNpadButton_Y, "Y" },
    { HidNpadButton_L, "L" },   { HidNpadButton_R, "R" },   { HidNpadButton_ZL, "ZL" },     { HidNpadButton_ZR, "ZR" },
    { HidNpadButton_Plus, "+" }, { HidNpadButton_Minus, "-" },
};
#define NUM_SWITCH_BUTTONS ((int)(sizeof(sSwitchButtons) / sizeof(sSwitchButtons[0])))

static const char *SwitchButtonName(u64 bit)
{
    for (int i = 0; i < NUM_SWITCH_BUTTONS; i++)
        if (sSwitchButtons[i].bit == bit)
            return sSwitchButtons[i].name;
    return "?";
}

static u64 SwitchButtonByName(const char *name)
{
    for (int i = 0; i < NUM_SWITCH_BUTTONS; i++)
        if (strcmp(sSwitchButtons[i].name, name) == 0)
            return sSwitchButtons[i].bit;
    return 0;
}

static u16 ReadController(void)
{
    u64 b = sButtonsHeld;
    u16 keys = 0;
    if (sHoldStartSelect) {
        if (!(b & (HidNpadButton_Plus | HidNpadButton_Minus)))
            sHoldStartSelect = false;
        b &= ~(u64)(HidNpadButton_Plus | HidNpadButton_Minus);
    }
    for (int i = 0; i < MAP_COUNT; i++)
        if (b & sMap[i])
            keys |= sMapGba[i];
    if (b & (HidNpadButton_Up | HidNpadButton_StickLUp))
        keys |= DPAD_UP;
    if (b & (HidNpadButton_Down | HidNpadButton_StickLDown))
        keys |= DPAD_DOWN;
    if (b & (HidNpadButton_Left | HidNpadButton_StickLLeft))
        keys |= DPAD_LEFT;
    if (b & (HidNpadButton_Right | HidNpadButton_StickLRight))
        keys |= DPAD_RIGHT;
    return keys;
}

/* ------------------------------------------------------------------------ */
/* Settings file: button map and cheats (settings.ini next to the save)     */
/* ------------------------------------------------------------------------ */

static const struct {
    const char *key;
    u8 *value;
} sCheatKeys[] = {
    { "infinite_lives", &gSonAdvCheats.infiniteLives },
    { "keep_rings", &gSonAdvCheats.keepRings },
    { "no_damage", &gSonAdvCheats.noDamage },
    { "all_zones", &gSonAdvCheats.allZones },
};
#define NUM_CHEATS ((int)(sizeof(sCheatKeys) / sizeof(sCheatKeys[0])))

static void LoadSettings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f == NULL) {
        LogF("settings: none yet, defaults\n");
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = 0;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        for (int i = 0; i < MAP_COUNT; i++) {
            u64 bit = SwitchButtonByName(val);
            if (strcmp(line, sMapKeys[i]) == 0 && bit != 0)
                sMap[i] = bit;
        }
        for (int i = 0; i < NUM_CHEATS; i++)
            if (strcmp(line, sCheatKeys[i].key) == 0)
                *sCheatKeys[i].value = (u8)(atoi(val) != 0);
    }
    fclose(f);
    LogF("settings: buttons A=%s B=%s L=%s R=%s Start=%s Select=%s; cheats lives %u rings %u damage %u stages %u\n",
         SwitchButtonName(sMap[0]), SwitchButtonName(sMap[1]), SwitchButtonName(sMap[2]), SwitchButtonName(sMap[3]),
         SwitchButtonName(sMap[4]), SwitchButtonName(sMap[5]), gSonAdvCheats.infiniteLives, gSonAdvCheats.keepRings,
         gSonAdvCheats.noDamage, gSonAdvCheats.allZones);
}

static void SaveSettings(void)
{
    FILE *f = fopen(SETTINGS_TMP, "w");
    if (f == NULL) {
        LogF("settings: cannot open %s (errno %d)\n", SETTINGS_TMP, errno);
        return;
    }
    for (int i = 0; i < MAP_COUNT; i++)
        fprintf(f, "%s=%s\n", sMapKeys[i], SwitchButtonName(sMap[i]));
    for (int i = 0; i < NUM_CHEATS; i++)
        fprintf(f, "%s=%u\n", sCheatKeys[i].key, *sCheatKeys[i].value);
    fclose(f);
    remove(SETTINGS_PATH);
    if (rename(SETTINGS_TMP, SETTINGS_PATH) != 0)
        LogF("settings: rename failed (errno %d)\n", errno);
    else
        LogF("settings: saved\n");
}

u16 Platform_GetKeyInput(void) { return ReadController(); }

/* ------------------------------------------------------------------------ */
/* Audio: SDL callback fed from a ring buffer, never cleared on backlog     */
/* ------------------------------------------------------------------------ */

#define AUDIO_RING_SAMPLES (48000 * 2 / 2) /* half a second of stereo s16 */
#define AUDIO_TARGET_FILL  (800 * 2 * 4)   /* about 4 frames */
#define AUDIO_MAX_FILL     (800 * 2 * 12)  /* drop input beyond 12 frames */

static s16 sAudioRing[AUDIO_RING_SAMPLES];
static volatile u32 sAudioRead, sAudioWrite; /* sample indices, monotonic */
static SDL_AudioDeviceID sAudioDevice;
static volatile u32 sAudioUnderruns, sAudioDrops;
static bool sAudioPrimed;

static void AudioCallback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    s16 *out = (s16 *)stream;
    u32 want = (u32)len / sizeof(s16);
    u32 rd = sAudioRead, wr = __atomic_load_n(&sAudioWrite, __ATOMIC_ACQUIRE);
    u32 avail = wr - rd;

    if (!sAudioPrimed) {
        if (avail < AUDIO_TARGET_FILL) {
            memset(stream, 0, len);
            return;
        }
        sAudioPrimed = true;
    }

    u32 n = avail < want ? avail : want;
    for (u32 i = 0; i < n; i++)
        out[i] = sAudioRing[(rd + i) % AUDIO_RING_SAMPLES];
    if (n < want) {
        /* Hold the last sample to avoid a click, then refill before resuming. */
        s16 l = n >= 2 ? out[n - 2] : 0, r = n >= 1 ? out[n - 1] : 0;
        for (u32 i = n; i < want; i += 2) {
            out[i] = l;
            if (i + 1 < want)
                out[i + 1] = r;
        }
        sAudioUnderruns++;
        sAudioPrimed = false;
    }
    __atomic_store_n(&sAudioRead, rd + n, __ATOMIC_RELEASE);
}

void Platform_QueueAudio(const s16 *data, u32 numBytes)
{
    if (sAudioDevice == 0)
        return;
    u32 count = numBytes / sizeof(s16);
    u32 rd = __atomic_load_n(&sAudioRead, __ATOMIC_ACQUIRE), wr = sAudioWrite;
    if ((wr - rd) + count > AUDIO_MAX_FILL) {
        sAudioDrops++;
        return; /* the game is ahead of the speaker; skip this frame's audio */
    }
    for (u32 i = 0; i < count; i++)
        sAudioRing[(wr + i) % AUDIO_RING_SAMPLES] = data[i];
    __atomic_store_n(&sAudioWrite, wr + count, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------ */
/* deko3d presentation                                                      */
/* ------------------------------------------------------------------------ */

#define FB_NUM     2
#define FB_WIDTH   1280
#define FB_HEIGHT  720
#define CODEMEM    (64 * 1024)
#define CMDMEM     (64 * 1024)

static DkDevice sDevice;
static DkMemBlock sFbMem, sCodeMem, sCmdMem, sTexMem, sDataMem;
static DkImage sFramebuffers[FB_NUM];
static DkSwapchain sSwapchain;
static DkShader sVsh, sFsh;
static DkCmdBuf sCmd;
static DkQueue sQueue;
static DkImage sTexture;
static DkImage sTextureClassic; /* 240x160: screens laid out for the GBA */
#define CLASSIC_W 240
#define CLASSIC_H 160
static u32 *sStaging;          /* RGBA8 frame, CPU-visible */
static DkGpuAddr sStagingGpu;
static DkGpuAddr sDescGpu;     /* image descriptor then sampler descriptor */
static u32 sDataSize;

ALIGNED(256) static uint16_t sGameImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static u32 sPalLut[0x8000];

static void LoadShader(DkShader *sh, const u8 *code, u32 size, u32 *offset)
{
    memcpy((u8 *)dkMemBlockGetCpuAddr(sCodeMem) + *offset, code, size);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, sCodeMem, *offset);
    dkShaderInitialize(sh, &m);
    *offset += (size + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
}

static void GfxInit(void)
{
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    sDevice = dkDeviceCreate(&dm);

    /* Framebuffers and swapchain */
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, sDevice);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FB_WIDTH;
    lm.dimensions[1] = FB_HEIGHT;
    DkImageLayout fbLayout;
    dkImageLayoutInitialize(&fbLayout, &lm);
    u32 fbSize = dkImageLayoutGetSize(&fbLayout);
    u32 fbAlign = dkImageLayoutGetAlignment(&fbLayout);
    fbSize = (fbSize + fbAlign - 1) & ~(fbAlign - 1);

    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, sDevice, FB_NUM * fbSize);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    sFbMem = dkMemBlockCreate(&mm);
    DkImage const *swImages[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        swImages[i] = &sFramebuffers[i];
        dkImageInitialize(&sFramebuffers[i], &fbLayout, sFbMem, i * fbSize);
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, sDevice, nwindowGetDefault(), swImages, FB_NUM);
    sSwapchain = dkSwapchainCreate(&sm);

    /* Shaders (compiled offline by uam, embedded in the NRO) */
    dkMemBlockMakerDefaults(&mm, sDevice, CODEMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    sCodeMem = dkMemBlockCreate(&mm);
    u32 codeOff = 0;
    LoadShader(&sVsh, present_vsh_dksh, present_vsh_dksh_len, &codeOff);
    LoadShader(&sFsh, present_fsh_dksh, present_fsh_dksh_len, &codeOff);

    /* Game texture (426x240 RGBA8) */
    dkImageLayoutMakerDefaults(&lm, sDevice);
    lm.flags = 0;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = DISPLAY_WIDTH;
    lm.dimensions[1] = DISPLAY_HEIGHT;
    DkImageLayout texLayout;
    dkImageLayoutInitialize(&texLayout, &lm);
    u32 texSize = dkImageLayoutGetSize(&texLayout);
    u32 texAlign = dkImageLayoutGetAlignment(&texLayout);
    u32 texBlock = (texSize + texAlign + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    lm.dimensions[0] = CLASSIC_W;
    lm.dimensions[1] = CLASSIC_H;
    DkImageLayout clsLayout;
    dkImageLayoutInitialize(&clsLayout, &lm);
    u32 clsOff = (texSize + dkImageLayoutGetAlignment(&clsLayout) - 1) & ~(dkImageLayoutGetAlignment(&clsLayout) - 1);
    texBlock = (clsOff + dkImageLayoutGetSize(&clsLayout) + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dkMemBlockMakerDefaults(&mm, sDevice, texBlock);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    sTexMem = dkMemBlockCreate(&mm);
    dkImageInitialize(&sTexture, &texLayout, sTexMem, 0);
    dkImageInitialize(&sTextureClassic, &clsLayout, sTexMem, clsOff);

    /* CPU-visible data: staging frame plus descriptors */
    u32 stagingSize = DISPLAY_WIDTH * DISPLAY_HEIGHT * 4;
    u32 descOff = (stagingSize + 255) & ~255u;
    sDataSize = (descOff + 256 + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dkMemBlockMakerDefaults(&mm, sDevice, sDataSize);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    sDataMem = dkMemBlockCreate(&mm);
    sStaging = (u32 *)dkMemBlockGetCpuAddr(sDataMem);
    sStagingGpu = dkMemBlockGetGpuAddr(sDataMem);
    sDescGpu = sStagingGpu + descOff;

    DkImageView texView;
    dkImageViewDefaults(&texView, &sTexture);
    DkImageDescriptor *imgDesc = (DkImageDescriptor *)((u8 *)sStaging + descOff);
    dkImageDescriptorInitialize(&imgDesc[0], &texView, false, false);
    DkImageView clsView;
    dkImageViewDefaults(&clsView, &sTextureClassic);
    dkImageDescriptorInitialize(&imgDesc[1], &clsView, false, false);
    DkSampler sampler;
    dkSamplerDefaults(&sampler); /* nearest filtering keeps pixels sharp */
    sampler.wrapMode[0] = DkWrapMode_ClampToEdge;
    sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
    DkSamplerDescriptor *smpDesc = (DkSamplerDescriptor *)((u8 *)sStaging + descOff + 128);
    dkSamplerDescriptorInitialize(smpDesc, &sampler);

    /* Command buffer and queue */
    dkMemBlockMakerDefaults(&mm, sDevice, CMDMEM);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    sCmdMem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, sDevice);
    sCmd = dkCmdBufCreate(&cm);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, sDevice);
    qm.flags = DkQueueFlags_Graphics;
    sQueue = dkQueueCreate(&qm);

    /* GBA BGR555 to RGBA8 lookup */
    for (u32 c = 0; c < 0x8000; c++) {
        u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        sPalLut[c] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    LogF("gfx: deko3d ready, %dx%d game picture on %dx%d swapchain\n", DISPLAY_WIDTH, DISPLAY_HEIGHT, FB_WIDTH, FB_HEIGHT);
}

static u64 sPresents;
static bool sClassicView;

static void GfxPresent(void)
{
    /* Wait for the previous frame's GPU work before reusing staging and
     * command memory (the work is a single quad; this costs almost nothing). */
    dkQueueWaitIdle(sQueue);

    /* Screens outside stages were laid out for the GBA's 240x160 (their
     * code is built with the original size): show that area centred. */
    bool classic = (gGameStageTask == NULL);
    if (classic != sClassicView) {
        sClassicView = classic;
        LogF("view: %s\n", classic ? "classic 240x160 (menus, intro, special stage)" : "widescreen (stage)");
    }

    if (classic) {
        for (u32 y = 0; y < CLASSIC_H; y++)
            for (u32 x = 0; x < CLASSIC_W; x++)
                sStaging[y * CLASSIC_W + x] = sPalLut[sGameImage[y * DISPLAY_WIDTH + x] & 0x7FFF];
    } else {
        for (u32 i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
            sStaging[i] = sPalLut[sGameImage[i] & 0x7FFF];
    }

    int slot = dkQueueAcquireImage(sQueue, sSwapchain); /* blocks until a buffer is free (vsync paced) */

    dkCmdBufClear(sCmd);
    dkCmdBufAddMemory(sCmd, sCmdMem, 0, CMDMEM);

    DkImageView texView;
    dkImageViewDefaults(&texView, classic ? &sTextureClassic : &sTexture);
    DkCopyBuf src = { sStagingGpu, 0, 0 };
    DkImageRect rect = { 0, 0, 0, classic ? CLASSIC_W : DISPLAY_WIDTH, classic ? CLASSIC_H : DISPLAY_HEIGHT, 1 };
    dkCmdBufCopyBufferToImage(sCmd, &src, &texView, &rect, 0);
    dkCmdBufBarrier(sCmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

    DkImageView fbView;
    dkImageViewDefaults(&fbView, &sFramebuffers[slot]);
    dkCmdBufBindRenderTarget(sCmd, &fbView, NULL);

    DkScissor sc = { 0, 0, FB_WIDTH, FB_HEIGHT };
    dkCmdBufSetScissors(sCmd, 0, &sc, 1);
    dkCmdBufClearColorFloat(sCmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    /* Classic: 240x160 at 4.5x = 1080x720, centred with black borders. */
    float vx = classic ? (FB_WIDTH - CLASSIC_W * FB_HEIGHT / CLASSIC_H) / 2.0f : 0.0f;
    float vw = classic ? (float)(CLASSIC_W * FB_HEIGHT / CLASSIC_H) : (float)FB_WIDTH;
    DkViewport vp = { vx, 0.0f, vw, (float)FB_HEIGHT, 0.0f, 1.0f };
    dkCmdBufSetViewports(sCmd, 0, &vp, 1);

    DkShader const *shaders[] = { &sVsh, &sFsh };
    dkCmdBufBindShaders(sCmd, DkStageFlag_GraphicsMask, shaders, 2);
    DkRasterizerState rs;
    DkColorState cs;
    DkColorWriteState cws;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkColorStateDefaults(&cs);
    dkColorWriteStateDefaults(&cws);
    dkCmdBufBindRasterizerState(sCmd, &rs);
    dkCmdBufBindColorState(sCmd, &cs);
    dkCmdBufBindColorWriteState(sCmd, &cws);
    dkCmdBufBindVtxAttribState(sCmd, NULL, 0);

    dkCmdBufBindImageDescriptorSet(sCmd, sDescGpu, 2);
    dkCmdBufBindSamplerDescriptorSet(sCmd, sDescGpu + 128, 1);
    dkCmdBufBindTexture(sCmd, DkStage_Fragment, 0, dkMakeTextureHandle(classic ? 1 : 0, 0));
    dkCmdBufDraw(sCmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);

    dkQueueSubmitCommands(sQueue, dkCmdBufFinishList(sCmd));
    dkQueuePresentImage(sQueue, sSwapchain, slot);
    sPresents++;
}

static void GfxExit(void)
{
    dkQueueWaitIdle(sQueue);
    dkQueueDestroy(sQueue);
    dkCmdBufDestroy(sCmd);
    dkMemBlockDestroy(sCmdMem);
    dkMemBlockDestroy(sDataMem);
    dkMemBlockDestroy(sTexMem);
    dkMemBlockDestroy(sCodeMem);
    dkSwapchainDestroy(sSwapchain);
    dkMemBlockDestroy(sFbMem);
    dkDeviceDestroy(sDevice);
}

/* ------------------------------------------------------------------------ */
/* Screen capture: right stick click saves the game picture as a BMP        */
/* ------------------------------------------------------------------------ */

static u32 sCaptures;

static void WriteLE(u8 *p, u32 v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (u8)(v >> (8 * i));
}

static void CaptureFrame(u64 frame)
{
    /* Saves what is on screen: the 240x160 area in classic view. */
    const int w = sClassicView ? CLASSIC_W : DISPLAY_WIDTH;
    const int h = sClassicView ? CLASSIC_H : DISPLAY_HEIGHT;
    char path[128];
    snprintf(path, sizeof(path), LOG_DIR "/capture_%03u_f%llu.bmp", ++sCaptures, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    u32 rowBytes = w * 3;
    u32 pad = (4 - (rowBytes & 3)) & 3;
    u32 dataSize = (rowBytes + pad) * h;
    u8 hdr[54] = { 'B', 'M' };
    WriteLE(hdr + 2, 54 + dataSize, 4);
    WriteLE(hdr + 10, 54, 4);
    WriteLE(hdr + 14, 40, 4);
    WriteLE(hdr + 18, w, 4);
    WriteLE(hdr + 22, h, 4);
    WriteLE(hdr + 26, 1, 2);
    WriteLE(hdr + 28, 24, 2);
    WriteLE(hdr + 34, dataSize, 4);
    fwrite(hdr, 1, 54, f);
    static u8 row[DISPLAY_WIDTH * 3 + 4];
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            u32 c = sPalLut[sGameImage[y * DISPLAY_WIDTH + x] & 0x7FFF];
            row[x * 3 + 0] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = c & 0xFF;
        }
        memset(row + rowBytes, 0, pad);
        fwrite(row, 1, rowBytes + pad, f);
    }
    fclose(f);
    LogF("capture: %s\n", path);
}

/* ------------------------------------------------------------------------ */
/* Instant replay: left stick click saves the last 2 seconds for debugging  */
/* ------------------------------------------------------------------------ */
/* Every frame the drawn game picture, OAM, I/O registers, palette and the
 * camera go into a ring of the last 120 frames (about 25 MB, allocated once).
 * Left stick click writes the ring, oldest frame first, to
 * logs/replay_NNN_fFRAME.bin; the game waits while it saves (a few seconds).
 * tools/replay/replay.py turns a file into PNGs. */

#define REPLAY_FRAMES 120
#define REPLAY_MAX_SAVES 10

typedef struct {
    u64 frame;
    u8 classic;
    u8 level;
    u8 character;
    u8 inStage;
    s32 camX, camY;
    u16 image[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    u8 oam[OAM_SIZE];
    u8 io[IO_SIZE];
    u16 pltt[PLTT_SIZE / 2];
} ReplayFrame;

static ReplayFrame *sReplay;
static u32 sReplayNext, sReplayCount, sReplaySaves;

static void ReplayInit(void)
{
    sReplay = (ReplayFrame *)malloc(sizeof(ReplayFrame) * REPLAY_FRAMES);
    LogF("replay: %s (%u frames, %u KB)\n", sReplay ? "ready, left stick click saves" : "no memory, off", REPLAY_FRAMES,
         (unsigned)(sizeof(ReplayFrame) * REPLAY_FRAMES / 1024));
}

static void ReplayRecord(u64 frame)
{
    if (sReplay == NULL)
        return;
    ReplayFrame *r = &sReplay[sReplayNext];
    r->frame = frame;
    r->classic = gGameStageTask == NULL;
    r->level = (u8)gCurrentLevel;
    r->character = (u8)gSelectedCharacter;
    r->inStage = gGameStageTask != NULL;
    r->camX = gCamera.x;
    r->camY = gCamera.y;
    memcpy(r->image, sGameImage, sizeof(r->image));
    memcpy(r->oam, OAM, sizeof(r->oam));
    memcpy(r->io, REG_BASE, sizeof(r->io));
    memcpy(r->pltt, PLTT, sizeof(r->pltt));
    sReplayNext = (sReplayNext + 1) % REPLAY_FRAMES;
    if (sReplayCount < REPLAY_FRAMES)
        sReplayCount++;
}

static void ReplaySave(u64 frame)
{
    if (sReplay == NULL || sReplayCount == 0)
        return;
    if (sReplaySaves >= REPLAY_MAX_SAVES) {
        LogF("replay: limit of %u per session reached\n", REPLAY_MAX_SAVES);
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), LOG_DIR "/replay_%03u_f%llu.bin", ++sReplaySaves, (unsigned long long)frame);
    u64 t0 = armGetSystemTick();
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        LogF("replay: cannot open %s (errno %d)\n", path, errno);
        return;
    }
    struct {
        char magic[8];
        u32 version, frames, frameBytes, width, height, oamSize, ioSize, plttSize;
    } hdr = { { 'S', 'A', 'N', 'X', 'R', 'P', 'L', '1' }, 1, sReplayCount, (u32)sizeof(ReplayFrame), DISPLAY_WIDTH,
              DISPLAY_HEIGHT, OAM_SIZE, IO_SIZE, PLTT_SIZE };
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    u32 first = (sReplayNext + REPLAY_FRAMES - sReplayCount) % REPLAY_FRAMES;
    for (u32 i = 0; i < sReplayCount && ok; i++)
        ok = fwrite(&sReplay[(first + i) % REPLAY_FRAMES], sizeof(ReplayFrame), 1, f) == 1;
    fclose(f);
    LogF("replay: %s %s, %u frames, %llu ms\n", ok ? "saved" : "FAILED writing", path, sReplayCount,
         (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ULL));
}

/* ------------------------------------------------------------------------ */
/* Port menu (Minus + Plus): drawn into the game picture with libnx's font  */
/* ------------------------------------------------------------------------ */

extern const u8 default_font_bin[]; /* libnx console font: 8x8 glyphs stored at 2x (16x16, 1bpp) */

static bool sMenuOpen;
/* The paused game's picture, composed once when the menu opens. Composing it
 * again every frame would re-run the game's per-line DMA transfers, walking
 * their source pointers off the end (garbled picture, then a crash). */
static uint16_t sMenuBackdrop[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static bool sExitRequested;
static bool sRestartRequested;
static bool sSettingsDirty;

/* Pages: main, cheats, controls. Rows are built each frame (Restart act only
 * shows during a single-player act). */
enum { PAGE_MAIN, PAGE_CHEATS, PAGE_CONTROLS, PAGE_SPECIAL };
enum {
    ROW_RESUME, ROW_RESTART, ROW_CONTROLS, ROW_CHEATS, ROW_EXIT,              /* main */
    ROW_CHEAT0, ROW_CHEAT_LAST = ROW_CHEAT0 + 3,                             /* cheats */
    ROW_MAP0, ROW_MAP_LAST = ROW_MAP0 + MAP_COUNT - 1, ROW_MAP_RESET,        /* controls */
    ROW_SPECIAL, ROW_SPECIAL0, ROW_SPECIAL_LAST = ROW_SPECIAL0 + SONADV_NUM_SPECIAL_STAGES - 1, /* special stages */
    ROW_BACK,
};
static int sSpecialRequested = -1;
static const char *const sCheatNames[NUM_CHEATS] = { "Infinite lives", "Keep rings", "No damage", "All stages" };

static int sMenuPage;
static int sMenuCursor;
static int sMenuRows[16];
static int sMenuRowCount;
static int sCaptureMap = -1;   /* controls: GBA button waiting for a Switch button */
static u64 sCaptureDeadline;   /* system tick when the wait gives up */

static void MenuBuildRows(void)
{
    sMenuRowCount = 0;
    if (sMenuPage == PAGE_MAIN) {
        sMenuRows[sMenuRowCount++] = ROW_RESUME;
        if (SonAdv_CanRestartAct()) {
            sMenuRows[sMenuRowCount++] = ROW_RESTART;
            sMenuRows[sMenuRowCount++] = ROW_SPECIAL;
        }
        sMenuRows[sMenuRowCount++] = ROW_CONTROLS;
        sMenuRows[sMenuRowCount++] = ROW_CHEATS;
        sMenuRows[sMenuRowCount++] = ROW_EXIT;
    } else if (sMenuPage == PAGE_CHEATS) {
        for (int i = 0; i < NUM_CHEATS; i++)
            sMenuRows[sMenuRowCount++] = ROW_CHEAT0 + i;
        sMenuRows[sMenuRowCount++] = ROW_BACK;
    } else if (sMenuPage == PAGE_SPECIAL) {
        for (int i = 0; i < SONADV_NUM_SPECIAL_STAGES; i++)
            sMenuRows[sMenuRowCount++] = ROW_SPECIAL0 + i;
        sMenuRows[sMenuRowCount++] = ROW_BACK;
    } else {
        for (int i = 0; i < MAP_COUNT; i++)
            sMenuRows[sMenuRowCount++] = ROW_MAP0 + i;
        sMenuRows[sMenuRowCount++] = ROW_MAP_RESET;
        sMenuRows[sMenuRowCount++] = ROW_BACK;
    }
    if (sMenuCursor >= sMenuRowCount)
        sMenuCursor = sMenuRowCount - 1;
}

static void MenuRowText(int row, char *left, char *right, size_t n)
{
    right[0] = 0;
    switch (row) {
        case ROW_RESUME: snprintf(left, n, "Resume"); break;
        case ROW_RESTART: snprintf(left, n, "Restart act"); break;
        case ROW_SPECIAL: snprintf(left, n, "Special stage"); break;
        case ROW_CONTROLS: snprintf(left, n, "Controls"); break;
        case ROW_CHEATS: snprintf(left, n, "Cheats"); break;
        case ROW_EXIT: snprintf(left, n, "Exit game"); break;
        case ROW_MAP_RESET: snprintf(left, n, "Reset to defaults"); break;
        case ROW_BACK: snprintf(left, n, "Back"); break;
        default:
            if (row >= ROW_SPECIAL0 && row <= ROW_SPECIAL_LAST) {
                snprintf(left, n, "Special stage %d", row - ROW_SPECIAL0 + 1);
            } else if (row >= ROW_CHEAT0 && row <= ROW_CHEAT_LAST) {
                snprintf(left, n, "%s", sCheatNames[row - ROW_CHEAT0]);
                snprintf(right, n, "%s", *sCheatKeys[row - ROW_CHEAT0].value ? "On" : "Off");
            } else if (row >= ROW_MAP0 && row <= ROW_MAP_LAST) {
                int m = row - ROW_MAP0;
                snprintf(left, n, "GBA %s", sMapNames[m]);
                snprintf(right, n, "%s", sCaptureMap == m ? "press..." : SwitchButtonName(sMap[m]));
            }
            break;
    }
}

static void MenuFillRect(int x0, int y0, int w, int h, u16 color, int vw, int vh)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= vh)
            continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < vw)
                sGameImage[y * DISPLAY_WIDTH + x] = color;
    }
}

static void MenuDarken(int vw, int vh)
{
    for (int y = 0; y < vh; y++)
        for (int x = 0; x < vw; x++) {
            u16 c = sGameImage[y * DISPLAY_WIDTH + x];
            sGameImage[y * DISPLAY_WIDTH + x] = (u16)((c >> 1) & 0x3DEF);
        }
}

static void MenuText(int x, int y, const char *str, u16 color, int vw, int vh)
{
    for (; *str; str++, x += 8) {
        const u8 *g = default_font_bin + (u8)*str * 32;
        for (int gy = 0; gy < 8; gy++) {
            u16 row = (u16)(g[gy * 4] | (g[gy * 4 + 1] << 8));
            for (int gx = 0; gx < 8; gx++) {
                if (!((row >> (15 - gx * 2)) & 1)) /* leftmost pixel is the high bit */
                    continue;
                int px = x + gx, py = y + gy;
                if (px >= 0 && px < vw && py >= 0 && py < vh)
                    sGameImage[py * DISPLAY_WIDTH + px] = color;
            }
        }
    }
}

static void MenuDraw(bool classic)
{
    static const char *const titles[] = { "Sonic Advance NX", "Cheats", "Controls", "Special stage" };
    const int vw = classic ? 240 : DISPLAY_WIDTH, vh = classic ? 160 : DISPLAY_HEIGHT;
    const int rowH = 12;
    const int bw = 208, bh = 34 + sMenuRowCount * rowH + (sMenuPage == PAGE_CONTROLS ? 12 : 0);
    const int bx = (vw - bw) / 2, by = (vh - bh) / 2;
    const char *title = titles[sMenuPage];
    MenuDarken(vw, vh);
    MenuFillRect(bx, by, bw, bh, 0x2884, vw, vh);
    MenuFillRect(bx, by, bw, 1, 0x7FFF, vw, vh);
    MenuFillRect(bx, by + bh - 1, bw, 1, 0x7FFF, vw, vh);
    MenuText(bx + (bw - (int)strlen(title) * 8) / 2, by + 8, title, 0x03FF, vw, vh);
    for (int i = 0; i < sMenuRowCount; i++) {
        char full[40], right[40];
        MenuRowText(sMenuRows[i], full, right, sizeof(full));
        u16 color = (i == sMenuCursor) ? 0x7FFF : 0x5294;
        int ty = by + 26 + i * rowH;
        if (i == sMenuCursor)
            MenuText(bx + 8, ty, ">", 0x03FF, vw, vh);
        MenuText(bx + 20, ty, full, color, vw, vh);
        if (right[0])
            MenuText(bx + bw - 12 - (int)strlen(right) * 8, ty, right, color, vw, vh);
    }
    if (sMenuPage == PAGE_CONTROLS)
        MenuText(bx + 20, by + bh - 14, sCaptureMap >= 0 ? "Press a button" : "A: change  B: back", 0x5294, vw, vh);
}

static void MenuClose(void)
{
    sMenuOpen = false;
    sCaptureMap = -1;
    if (sSettingsDirty) {
        SaveSettings();
        sSettingsDirty = false;
    }
    if (sAudioDevice != 0)
        SDL_PauseAudioDevice(sAudioDevice, 0);
    LogF("menu: %s\n", sExitRequested ? "exit game" : sRestartRequested ? "restart act"
                       : sSpecialRequested >= 0 ? "special stage" : "resume");
}

/* Opens a page with the cursor on the given row (first row if absent). */
static void MenuGoPage(int page, int row)
{
    sMenuPage = page;
    sMenuCursor = 0;
    MenuBuildRows();
    for (int i = 0; i < sMenuRowCount; i++)
        if (sMenuRows[i] == row)
            sMenuCursor = i;
}

/* Controls page, waiting for a button: the first allowed Switch button pressed
 * becomes the GBA button's; a GBA button that had it takes the old one (swap). */
static void MenuCaptureUpdate(void)
{
    for (int i = 0; i < NUM_SWITCH_BUTTONS; i++) {
        u64 bit = sSwitchButtons[i].bit;
        if (!(sButtonsDown & bit))
            continue;
        u64 old = sMap[sCaptureMap];
        for (int m = 0; m < MAP_COUNT; m++)
            if (m != sCaptureMap && sMap[m] == bit)
                sMap[m] = old;
        sMap[sCaptureMap] = bit;
        LogF("controls: GBA %s on Switch %s\n", sMapNames[sCaptureMap], sSwitchButtons[i].name);
        sSettingsDirty = true;
        sCaptureMap = -1;
        return;
    }
    if (armGetSystemTick() > sCaptureDeadline)
        sCaptureMap = -1;
}

/* Returns true while the menu holds the game paused. */
static bool MenuUpdate(void)
{
    const u64 combo = HidNpadButton_Minus | HidNpadButton_Plus;
    if (!sMenuOpen) {
        if ((sButtonsHeld & combo) == combo && (sButtonsDown & combo)) {
            sMenuOpen = true;
            sHoldStartSelect = true;
            MenuGoPage(PAGE_MAIN, ROW_RESUME);
            if (sAudioDevice != 0)
                SDL_PauseAudioDevice(sAudioDevice, 1);
            LogF("menu: open\n");
        }
        return sMenuOpen;
    }
    if (sCaptureMap >= 0) {
        MenuCaptureUpdate();
        return true;
    }
    MenuBuildRows();
    if (sButtonsDown & (HidNpadButton_Up | HidNpadButton_StickLUp))
        sMenuCursor = (sMenuCursor + sMenuRowCount - 1) % sMenuRowCount;
    if (sButtonsDown & (HidNpadButton_Down | HidNpadButton_StickLDown))
        sMenuCursor = (sMenuCursor + 1) % sMenuRowCount;

    if (sButtonsDown & HidNpadButton_B) {
        if (sMenuPage == PAGE_MAIN) {
            MenuClose();
            return false;
        }
        MenuGoPage(PAGE_MAIN, sMenuPage == PAGE_CHEATS ? ROW_CHEATS : sMenuPage == PAGE_SPECIAL ? ROW_SPECIAL : ROW_CONTROLS);
        return true;
    }
    bool left = (sButtonsDown & (HidNpadButton_Left | HidNpadButton_StickLLeft)) != 0;
    bool right = (sButtonsDown & (HidNpadButton_Right | HidNpadButton_StickLRight)) != 0;
    bool press = (sButtonsDown & HidNpadButton_A) != 0;
    int row = sMenuRows[sMenuCursor];

    if (row >= ROW_CHEAT0 && row <= ROW_CHEAT_LAST && (press || left || right)) {
        u8 *v = sCheatKeys[row - ROW_CHEAT0].value;
        *v = !*v;
        sSettingsDirty = true;
        LogF("cheats: %s %s\n", sCheatKeys[row - ROW_CHEAT0].key, *v ? "on" : "off");
        return true;
    }
    if (!press)
        return true;
    switch (row) {
        case ROW_RESUME: MenuClose(); return false;
        case ROW_RESTART: sRestartRequested = true; MenuClose(); return false;
        case ROW_EXIT: sExitRequested = true; MenuClose(); return true;
        case ROW_CONTROLS: MenuGoPage(PAGE_CONTROLS, ROW_MAP0); return true;
        case ROW_CHEATS: MenuGoPage(PAGE_CHEATS, ROW_CHEAT0); return true;
        case ROW_SPECIAL: MenuGoPage(PAGE_SPECIAL, ROW_SPECIAL0); return true;
        case ROW_BACK:
            MenuGoPage(PAGE_MAIN, sMenuPage == PAGE_CHEATS ? ROW_CHEATS : sMenuPage == PAGE_SPECIAL ? ROW_SPECIAL : ROW_CONTROLS);
            return true;
        case ROW_MAP_RESET:
            memcpy(sMap, sMapDefault, sizeof(sMap));
            sSettingsDirty = true;
            LogF("controls: reset to defaults\n");
            return true;
        default:
            if (row >= ROW_SPECIAL0 && row <= ROW_SPECIAL_LAST) {
                sSpecialRequested = row - ROW_SPECIAL0;
                MenuClose();
                return false;
            }
            if (row >= ROW_MAP0 && row <= ROW_MAP_LAST) {
                sCaptureMap = row - ROW_MAP0;
                sCaptureDeadline = armGetSystemTick() + armNsToTicks(6000000000ULL);
            }
            return true;
    }
}

/* ------------------------------------------------------------------------ */
/* Heartbeat                                                                */
/* ------------------------------------------------------------------------ */

static u64 sFrames;
static u64 sLastBeatTick, sLastBeatFrames, sLastBeatPresents;
static u64 sBusyTicks; /* game thread time spent outside the vsync wait */

static void Heartbeat(void)
{
    u64 now = armGetSystemTick();
    if (sLastBeatTick == 0) {
        sLastBeatTick = now;
        return;
    }
    u64 dt = now - sLastBeatTick;
    if (dt < armGetSystemTickFreq() * 5)
        return;
    double secs = (double)dt / armGetSystemTickFreq();
    u64 df = sFrames - sLastBeatFrames, dp = sPresents - sLastBeatPresents;
    LogF("beat: frames %llu (%.1f/s) presents %.1f/s game busy %.0f%% audio fill %u underruns %u drops %u\n",
         (unsigned long long)sFrames, df / secs, dp / secs, 100.0 * sBusyTicks / dt,
         (unsigned)(sAudioWrite - sAudioRead), (unsigned)sAudioUnderruns, (unsigned)sAudioDrops);
    sLastBeatTick = now;
    sLastBeatFrames = sFrames;
    sLastBeatPresents = sPresents;
    sBusyTicks = 0;
    static u32 beats;
    if ((++beats % 6) == 0)
        LogClocks("periodic");
}

/* ------------------------------------------------------------------------ */
/* Frame hook: the game calls this once per pass of its main loop           */
/* ------------------------------------------------------------------------ */

void DoSoftReset(void) {}

extern void WideBg_Update(void);

void VDraw(void)
{
    WideBg_Update();
    gpsp_draw_frame(sGameImage);
    REG_VCOUNT = DISPLAY_HEIGHT + 1;
}

static void HandleVBlankInterrupts(void)
{
    REG_DISPSTAT |= INTR_FLAG_VBLANK;
    RunDMAs(DMA_VBLANK);
    if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
        gIntrTable[INTR_INDEX_VBLANK]();
    REG_DISPSTAT &= ~INTR_FLAG_VBLANK;
}

static AppletHookCookie sAppletCookie;

/* Each step is logged (and flushed) so a failure at exit shows where it was.
 * The SD card stays mounted and stdio stays open: libnx and newlib close them
 * after this, and the crash handler still needs the card. */
static void Shutdown(void)
{
    LogF("exit: frames %llu, saves written %u\n", (unsigned long long)sFrames, sSaveWrites);
    FlushSaveIfChanged(false);
    appletUnhook(&sAppletCookie);
    if (sAudioDevice != 0) {
        SDL_PauseAudioDevice(sAudioDevice, 1);
        SDL_CloseAudioDevice(sAudioDevice);
        sAudioDevice = 0;
    }
    LogF("exit: audio closed\n");
    SDL_Quit();
    LogF("exit: SDL done\n");
    GfxExit();
    LogF("exit: graphics done, ending through AM\n");
    fflush(stdout);
    fflush(stderr);
    sLoaderReturn = envGetExitFuncPtr();
    __nx_applet_exit_mode = 1;
    exit(0);
}

static u64 sFrameStartTick;

void VBlankIntrWait(void)
{
    u64 t0 = armGetSystemTick();
    if (sFrameStartTick != 0)
        sBusyTicks += t0 - sFrameStartTick;

    if (!appletMainLoop())
        Shutdown();

    padUpdate(&sPad);
    sButtonsHeld = padGetButtons(&sPad);
    sButtonsDown = padGetButtonsDown(&sPad);

    /* Port menu: the game stays paused (this call does not return to it)
     * until the menu closes. */
    bool backdropReady = false;
    while (MenuUpdate()) {
        if (sExitRequested)
            Shutdown();
        if (!backdropReady) {
            VDraw();
            memcpy(sMenuBackdrop, sGameImage, sizeof(sMenuBackdrop));
            backdropReady = true;
        }
        memcpy(sGameImage, sMenuBackdrop, sizeof(sMenuBackdrop));
        MenuDraw(gGameStageTask == NULL);
        GfxPresent();
        if (!appletMainLoop())
            Shutdown();
        padUpdate(&sPad);
        sButtonsHeld = padGetButtons(&sPad);
        sButtonsDown = padGetButtonsDown(&sPad);
    }
    if (sExitRequested)
        Shutdown();
    if (sRestartRequested) {
        sRestartRequested = false;
        LogF("stage: restart act (level %d)\n", (int)gCurrentLevel);
        SonAdv_RestartAct();
    }
    if (sSpecialRequested >= 0) {
        LogF("stage: special stage %d (from level %d)\n", sSpecialRequested + 1, (int)gCurrentLevel);
        SonAdv_EnterSpecialStage(sSpecialRequested);
        sSpecialRequested = -1;
    }

    VDraw();
    ReplayRecord(sFrames);
    if (sButtonsDown & HidNpadButton_StickR)
        CaptureFrame(sFrames);
    if (sButtonsDown & HidNpadButton_StickL)
        ReplaySave(sFrames);
    GfxPresent();

    REG_KEYINPUT = KEYS_MASK ^ ReadController();
    HandleVBlankInterrupts();

    FlushSaveIfChanged(false);
    sFrames++;
    Heartbeat();
    sFrameStartTick = armGetSystemTick();
}

/* ------------------------------------------------------------------------ */
/* Entry                                                                    */
/* ------------------------------------------------------------------------ */

static void AppletHook(AppletHookType hook, void *param)
{
    (void)param;
    switch (hook) {
        case AppletHookType_OnFocusState:
            LogF("applet: focus %s\n", appletGetFocusState() == AppletFocusState_InFocus ? "in" : "out");
            break;
        case AppletHookType_OnExitRequest:
            LogF("applet: exit requested\n");
            break;
        default:
            break;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sModuleBase = FindModuleBase();

    OpenLogs();
    LogF("module base 0x%llx (crash offsets are relative to it)\n", (unsigned long long)sModuleBase);
    LogF("applet type %d\n", (int)appletGetAppletType());
    clkrstInitialize();
    LogClocks("start");
    appletHook(&sAppletCookie, AppletHook, NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&sPad);

    /* Game data comes from the player's own ROM; the NRO ships without it. */
    {
        static SonAdvRomInfo rom;
        if (SonAdv_RomFill(&rom) != 0) {
            LogF("rom: FAILED: %s\n", rom.error);
            consoleInit(NULL);
            printf("\n  Sonic Advance NX %s\n\n%s\n\n  Press + to exit.\n", SONADV_VERSION, rom.error);
            consoleUpdate(NULL);
            while (appletMainLoop()) {
                padUpdate(&sPad);
                if (padGetButtonsDown(&sPad) & HidNpadButton_Plus) break;
                svcSleepThread(16000000);
            }
            consoleExit(NULL);
            return 0;
        }
        LogF("rom: %s (%s), %zu bytes filled from the ROM, %zu from the USA patch, %zu skipped, crc %08x ok\n",
             rom.romName, rom.region == 1 ? "Europe" : "USA", rom.copied, rom.literal, rom.skipped, rom.crc);
    }

    ReadSaveFile();
    LoadSettings();
    ReplayInit();

    /* Keep the original multiplayer screen from claiming the display. */
    REG_RCNT = 0x8000;
    REG_KEYINPUT = KEYS_MASK;

    GfxInit();

#if ENABLE_AUDIO
    if (SDL_Init(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 48000;
        want.format = AUDIO_S16;
        want.channels = 2;
        want.samples = 1024;
        want.callback = AudioCallback;
        cgb_audio_init(want.freq);
        sAudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (sAudioDevice != 0) {
            SDL_PauseAudioDevice(sAudioDevice, 0);
            LogF("audio: %d Hz, %d ch, %d sample buffer\n", have.freq, have.channels, have.samples);
        } else {
            LogF("audio: open failed: %s\n", SDL_GetError());
        }
    } else {
        LogF("audio: SDL init failed: %s\n", SDL_GetError());
    }
#endif

    LogF("starting game\n");
    AgbMain();
    Shutdown();
    return 0;
}
