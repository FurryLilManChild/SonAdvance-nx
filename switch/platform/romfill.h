// SonAdvance-nx: fill the game's data from the player's own ROM (romfill.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int region;          // 1 Europe, 2 USA, 0 none
    char romName[256];
    size_t copied;       // bytes copied from the ROM
    size_t literal;      // bytes from the USA patch (USA ROM only)
    size_t skipped;      // Europe-only link-cable data left empty (USA ROM only)
    uint32_t crc;
    int crcOk;
    char error[512];     // set when SonAdv_RomFill fails
} SonAdvRomInfo;

// Returns 0 when the game data is in place, -1 with info->error set otherwise.
int SonAdv_RomFill(SonAdvRomInfo *info);
