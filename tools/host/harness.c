/*
 * Host test harness (Linux, headless): drives the game with a button script
 * and saves frames as PPM files, running as fast as the CPU allows.
 * Not part of the Switch build.
 *
 * Environment:
 *   SCRIPT=file   lines "<frame> <keys>": keys held from that frame on.
 *                 keys: A B S(start) E(select) L R U D < > or '-' for none.
 *                 A line "<frame> end" stops the run.
 *   CAPS=list     comma list of frames to save, or "every:N".
 *   CAPDIR=dir    where frames go (default ./caps).
 *   LAYERS=hex    DISPCNT layer mask to keep while drawing (default 0x1F00).
 *   RESULTS_AT=frame  show the end-of-act results (use with LEVEL).
 *   SPECIAL_AT=frame  enter the special stage (use with LEVEL).
 *   RESTART_AT=frame  restart the act (the port menu action).
 *   CHEATS=list   any of lives,rings,damage,zones,super (comma separated).
 *   HIT_AT=frame  hit the player once (Coll_DamagePlayer). KILL_AT=frame: lose a life.
 *   CHARSEL_AT=frame  open the character select (single player).
 *   COURSE_AT=frame  open the course select (single player, character CHAR).
 *   STATUS=1      print rings, lives and level every 60 frames.
 *   LEVEL=n LEVEL_AT=frame CHAR=c   jump straight into level n (0..13) at that
 *                 frame as character c (0 Sonic, 1 Tails, 2 Knuckles, 3 Amy).
 * Frame files are named f<frame>s.ppm when a stage is running, else f<frame>m.ppm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "core.h"
#include "flags.h"
#include "platform/shared/video/gpsp_renderer.h"
#include "task.h"
#include "game/globals.h"
#include "game/shared/stage/stage.h"
#include "game/sa1/special_stage/main.h"
#include "sonadv_port.h"
#include "game/shared/stage/player.h"

extern void ApplyGameStageSettingsAndStart(void);

#define MAX_STEPS 4096

static struct {
    u32 frame;
    u16 keys;
    int end;
} sSteps[MAX_STEPS];
static int sNumSteps, sStep;
static u32 sFrame;
static u32 sCapEvery;
static u32 sCaps[1024];
static int sNumCaps;
static const char *sCapDir = "caps";
static u16 sLayerMask = 0x1F00;
static int sInit;
static int sLevel = -1, sLevelAt = -1, sChar, sSpecialAt = -1, sResultsAt = -1, sRestartAt = -1, sStatus, sHitAt = -1, sKillAt = -1, sCourseAt = -1;
static uint16_t sImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];

static u16 ParseKeys(const char *s)
{
    u16 k = 0;
    for (; *s; s++) {
        switch (*s) {
            case 'A': k |= A_BUTTON; break;
            case 'B': k |= B_BUTTON; break;
            case 'S': k |= START_BUTTON; break;
            case 'E': k |= SELECT_BUTTON; break;
            case 'L': k |= L_BUTTON; break;
            case 'R': k |= R_BUTTON; break;
            case 'U': k |= DPAD_UP; break;
            case 'D': k |= DPAD_DOWN; break;
            case '<': k |= DPAD_LEFT; break;
            case '>': k |= DPAD_RIGHT; break;
            default: break;
        }
    }
    return k;
}

static void DumpAnimPalettes(void)
{
    static const int anims[] = { 0, 398, 399, 400 };
    for (unsigned i = 0; i < sizeof(anims) / sizeof(anims[0]); i++) {
        const s32 *c = (const s32 *)gRefSpriteTables->animations[anims[i]][0];
        for (int w = 0; w < 64; w++) {
            if (c[w] == -2) {
                s32 pal = c[w + 1];
                u16 n = ((const u16 *)&c[w + 2])[0], off = ((const u16 *)&c[w + 2])[1];
                fprintf(stderr, "anim %d: palette %d colors %u at %u:", anims[i], pal, n, off);
                for (int k = 0; k < n && k < 16; k++)
                    fprintf(stderr, " %04x", gRefSpriteTables->palettes[pal * 16 + k]);
                fprintf(stderr, "\n");
                break;
            }
        }
    }
}

static void Init(void)
{
    sInit = 1;
    const char *script = getenv("SCRIPT");
    if (script) {
        FILE *f = fopen(script, "r");
        char line[256], keys[64];
        unsigned fr;
        while (f && fgets(line, sizeof line, f) && sNumSteps < MAX_STEPS) {
            if (line[0] == '#' || sscanf(line, "%u %63s", &fr, keys) != 2)
                continue;
            sSteps[sNumSteps].frame = fr;
            sSteps[sNumSteps].end = strcmp(keys, "end") == 0;
            sSteps[sNumSteps].keys = ParseKeys(keys);
            sNumSteps++;
        }
        if (f)
            fclose(f);
    }
    const char *caps = getenv("CAPS");
    if (caps) {
        if (strncmp(caps, "every:", 6) == 0) {
            sCapEvery = (u32)atoi(caps + 6);
        } else {
            char buf[8192];
            strncpy(buf, caps, sizeof buf - 1);
            for (char *t = strtok(buf, ","); t && sNumCaps < 1024; t = strtok(NULL, ","))
                sCaps[sNumCaps++] = (u32)atoi(t);
        }
    }
    if (getenv("CAPDIR"))
        sCapDir = getenv("CAPDIR");
    if (getenv("LEVEL")) {
        sLevel = atoi(getenv("LEVEL"));
        sLevelAt = getenv("LEVEL_AT") ? atoi(getenv("LEVEL_AT")) : 400;
        sChar = getenv("CHAR") ? atoi(getenv("CHAR")) : 0;
    }
    if (getenv("RESULTS_AT"))
        sResultsAt = atoi(getenv("RESULTS_AT"));
    if (getenv("RESTART_AT"))
        sRestartAt = atoi(getenv("RESTART_AT"));
    if (getenv("HIT_AT"))
        sHitAt = atoi(getenv("HIT_AT"));
    if (getenv("KILL_AT"))
        sKillAt = atoi(getenv("KILL_AT"));
    if (getenv("COURSE_AT")) {
        sCourseAt = atoi(getenv("COURSE_AT"));
        sChar = getenv("CHAR") ? atoi(getenv("CHAR")) : 0;
    }
    if (getenv("DUMPPAL"))
        DumpAnimPalettes();
    if (getenv("STATUS"))
        sStatus = 1;
    if (getenv("CHEATS")) {
        const char *c = getenv("CHEATS");
        gSonAdvCheats.infiniteLives = strstr(c, "lives") != NULL;
        gSonAdvCheats.keepRings = strstr(c, "rings") != NULL;
        gSonAdvCheats.noDamage = strstr(c, "damage") != NULL;
        gSonAdvCheats.allZones = strstr(c, "zones") != NULL;
        gSonAdvCheats.superSonic = strstr(c, "super") != NULL;
    }
    if (getenv("SPECIAL_AT"))
        sSpecialAt = atoi(getenv("SPECIAL_AT"));
    if (getenv("LAYERS"))
        sLayerMask = (u16)strtoul(getenv("LAYERS"), NULL, 16);
}

static int WantCap(u32 frame)
{
    if (sCapEvery && frame % sCapEvery == 0)
        return 1;
    for (int i = 0; i < sNumCaps; i++)
        if (sCaps[i] == frame)
            return 1;
    return 0;
}

extern void WideBg_Update(void);

static void SaveFrame(u32 frame)
{
    WideBg_Update();
    u16 saved = REG_DISPCNT;
    REG_DISPCNT = (saved & ~0x1F00) | (saved & sLayerMask);
    gpsp_draw_frame(sImage);
    REG_DISPCNT = saved;

    char path[512];
    snprintf(path, sizeof path, "%s/f%06u%c.ppm", sCapDir, frame, gGameStageTask != NULL ? 's' : 'm');
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    /* Outside stages the port shows the 240x160 area: save that. */
    int classic = (gGameStageTask == NULL) && !getenv("NOCROP");
    int w = classic ? 240 : DISPLAY_WIDTH, h = classic ? 160 : DISPLAY_HEIGHT;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        u16 c = sImage[(i / w) * DISPLAY_WIDTH + (i % w)];
        u8 rgb[3] = { (u8)((c & 0x1F) << 3), (u8)(((c >> 5) & 0x1F) << 3), (u8)(((c >> 10) & 0x1F) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Called from the headless VBlankIntrWait, once per game frame. */
void Harness_Frame(void)
{
    if (!sInit)
        Init();

    while (sStep < sNumSteps && sSteps[sStep].frame <= sFrame) {
        if (sSteps[sStep].end) {
            extern u8 FLASH_BASE[];
            u32 used = 0;
            for (u32 i = 0; i < 64 * 1024; i++)
                used += FLASH_BASE[i] != 0xFF;
            fprintf(stderr, "harness: flash bytes in use %u, no-flash flag %d\n", used, (gFlags & FLAGS_NO_FLASH_MEMORY) != 0);
            fprintf(stderr, "harness: end at frame %u\n", sFrame);
            exit(0);
        }
        sStep++;
    }
    u16 keys = sStep > 0 ? sSteps[sStep - 1].keys : 0;
    REG_KEYINPUT = KEYS_MASK ^ keys;

    if ((int)sFrame == sLevelAt && sLevel >= 0) {
        TasksDestroyAll();
        gGameMode = GAME_MODE_SINGLE_PLAYER;
        gSelectedCharacter = sChar;
        gCurrentLevel = sLevel;
        ApplyGameStageSettingsAndStart();
        fprintf(stderr, "harness: started level %d at frame %u\n", sLevel, sFrame);
    }
    if ((int)sFrame == sRestartAt) {
        fprintf(stderr, "harness: restart act at frame %u (can %d)\n", sFrame, (int)SonAdv_CanRestartAct());
        SonAdv_RestartAct();
    }
    if ((int)sFrame == sCourseAt) {
        extern void CreateCourseSelect(bool8 arg0);
        TasksDestroyAll();
        PAUSE_BACKGROUNDS_QUEUE();
        gBgSpritesCount = 0;
        PAUSE_GRAPHICS_QUEUE();
        gGameMode = GAME_MODE_SINGLE_PLAYER;
        gSelectedCharacter = sChar;
        CreateCourseSelect(FALSE);
        fprintf(stderr, "harness: course select at frame %u\n", sFrame);
    }
    if (getenv("CHARSEL_AT") && (int)sFrame == atoi(getenv("CHARSEL_AT"))) {
        extern void CreateCharacterSelectionScreen(u8 character);
        TasksDestroyAll();
        PAUSE_BACKGROUNDS_QUEUE();
        gBgSpritesCount = 0;
        PAUSE_GRAPHICS_QUEUE();
        gGameMode = GAME_MODE_SINGLE_PLAYER;
        CreateCharacterSelectionScreen(0);
        fprintf(stderr, "harness: character select at frame %u\n", sFrame);
    }
    if ((int)sFrame == sHitAt) {
        extern bool32 Coll_DamagePlayer(Player * p);
        int r = Coll_DamagePlayer(&gPlayer);
        fprintf(stderr, "harness: hit at frame %u -> %d, rings now %u\n", sFrame, r, (unsigned)gRingCount);
    }
    if ((int)sFrame == sKillAt) {
        gPlayer.moveState |= MOVESTATE_DEAD;
        fprintf(stderr, "harness: player dead at frame %u\n", sFrame);
    }
    if (sStatus && (sFrame % 60) == 0)
        fprintf(stderr, "status f%u: level %d rings %u lives %u time %u stage %d\n", sFrame, (int)gCurrentLevel,
                (unsigned)gRingCount, (unsigned)gNumLives, (unsigned)gCourseTime, gGameStageTask != NULL);
    if ((int)sFrame == sResultsAt) {
        extern u16 CreateStageResults(u32 courseTime, u16 ringCount, u8 spRingCount);
        CreateStageResults(gCourseTime, gRingCount, 0);
        fprintf(stderr, "harness: stage results at frame %u\n", sFrame);
    }
    if ((int)sFrame == sSpecialAt) {
        // Same steps the special spring takes (special_spring.c).
        TasksDestroyAll();
        PAUSE_BACKGROUNDS_QUEUE();
        gBgSpritesCount = 0;
        PAUSE_GRAPHICS_QUEUE();
        CreateSpecialStageIntro();
        fprintf(stderr, "harness: special stage at frame %u\n", sFrame);
    }
    if (getenv("OAMDUMP") && (int)sFrame == atoi(getenv("OAMDUMP"))) {
        // Hardware sprite table at this frame (attr0 attr1 attr2), visible entries only.
        for (int i = 0; i < 128; i++) {
            const u16 *o = (const u16 *)&OAM[i * (OAM_SIZE / 128)];
            if ((o[0] & 0x0300) == 0x0200)
                continue; /* disabled */
            fprintf(stderr, "oam %3d: %04x %04x %04x  y=%d x=%d tile=%d\n", i, o[0], o[1], o[2], o[0] & 0xFF, o[1] & 0x1FF, o[2] & 0x3FF);
        }
    }
    if (WantCap(sFrame))
        SaveFrame(sFrame);
    sFrame++;
}
