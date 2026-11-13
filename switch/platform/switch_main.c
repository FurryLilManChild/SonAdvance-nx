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
#include <deko3d.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "global.h"
#include "core.h"
#include "lib/agb_flash/flash_internal.h"
#include "platform/shared/dma.h"
#include "platform/shared/video/gpsp_renderer.h"

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

/* Default: Nintendo Switch Online GBA layout (A is A, B is B). */
static u16 ReadController(void)
{
    u64 b = sButtonsHeld;
    u16 keys = 0;
    if (b & HidNpadButton_A)
        keys |= A_BUTTON;
    if (b & HidNpadButton_B)
        keys |= B_BUTTON;
    if (b & HidNpadButton_Plus)
        keys |= START_BUTTON;
    if (b & HidNpadButton_Minus)
        keys |= SELECT_BUTTON;
    if (b & HidNpadButton_L)
        keys |= L_BUTTON;
    if (b & HidNpadButton_R)
        keys |= R_BUTTON;
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
    dkMemBlockMakerDefaults(&mm, sDevice, texBlock);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    sTexMem = dkMemBlockCreate(&mm);
    dkImageInitialize(&sTexture, &texLayout, sTexMem, 0);

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
    dkImageDescriptorInitialize(imgDesc, &texView, false, false);
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

static void GfxPresent(void)
{
    /* Wait for the previous frame's GPU work before reusing staging and
     * command memory (the work is a single quad; this costs almost nothing). */
    dkQueueWaitIdle(sQueue);

    for (u32 i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        sStaging[i] = sPalLut[sGameImage[i] & 0x7FFF];

    int slot = dkQueueAcquireImage(sQueue, sSwapchain); /* blocks until a buffer is free (vsync paced) */

    dkCmdBufClear(sCmd);
    dkCmdBufAddMemory(sCmd, sCmdMem, 0, CMDMEM);

    DkImageView texView;
    dkImageViewDefaults(&texView, &sTexture);
    DkCopyBuf src = { sStagingGpu, 0, 0 };
    DkImageRect rect = { 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 1 };
    dkCmdBufCopyBufferToImage(sCmd, &src, &texView, &rect, 0);
    dkCmdBufBarrier(sCmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

    DkImageView fbView;
    dkImageViewDefaults(&fbView, &sFramebuffers[slot]);
    dkCmdBufBindRenderTarget(sCmd, &fbView, NULL);

    DkViewport vp = { 0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT, 0.0f, 1.0f };
    DkScissor sc = { 0, 0, FB_WIDTH, FB_HEIGHT };
    dkCmdBufSetViewports(sCmd, 0, &vp, 1);
    dkCmdBufSetScissors(sCmd, 0, &sc, 1);
    dkCmdBufClearColorFloat(sCmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

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

    dkCmdBufBindImageDescriptorSet(sCmd, sDescGpu, 1);
    dkCmdBufBindSamplerDescriptorSet(sCmd, sDescGpu + 128, 1);
    dkCmdBufBindTexture(sCmd, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
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
    char path[128];
    snprintf(path, sizeof(path), LOG_DIR "/capture_%03u_f%llu.bmp", ++sCaptures, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return;
    u32 rowBytes = DISPLAY_WIDTH * 3;
    u32 pad = (4 - (rowBytes & 3)) & 3;
    u32 dataSize = (rowBytes + pad) * DISPLAY_HEIGHT;
    u8 hdr[54] = { 'B', 'M' };
    WriteLE(hdr + 2, 54 + dataSize, 4);
    WriteLE(hdr + 10, 54, 4);
    WriteLE(hdr + 14, 40, 4);
    WriteLE(hdr + 18, DISPLAY_WIDTH, 4);
    WriteLE(hdr + 22, DISPLAY_HEIGHT, 4);
    WriteLE(hdr + 26, 1, 2);
    WriteLE(hdr + 28, 24, 2);
    WriteLE(hdr + 34, dataSize, 4);
    fwrite(hdr, 1, 54, f);
    static u8 row[DISPLAY_WIDTH * 3 + 4];
    for (int y = DISPLAY_HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
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

void VDraw(void)
{
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

static void Shutdown(void)
{
    LogF("exit: frames %llu, saves written %u\n", (unsigned long long)sFrames, sSaveWrites);
    FlushSaveIfChanged(false);
    if (sAudioDevice != 0)
        SDL_CloseAudioDevice(sAudioDevice);
    SDL_Quit();
    GfxExit();
    if (sLog != NULL)
        fclose(sLog);
    fsdevUnmountAll();
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

    VDraw();
    if (sButtonsDown & HidNpadButton_StickR)
        CaptureFrame(sFrames);
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

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&sPad);

    ReadSaveFile();

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
