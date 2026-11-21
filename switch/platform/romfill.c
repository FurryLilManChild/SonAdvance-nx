// SonAdvance-nx: fill the game's data from the player's own ROM.
//
// The NRO ships without game data: switch/tools/romfill.py zeroes every piece
// of the ELF that comes from the ROM and stores a table of positions in RomFS
// (romfs:/romfill.bin). At startup this reads the player's dump from the app
// folder, identifies it by SHA-1 (Europe or USA), copies each piece back and
// checks the result against the CRC the build tool computed.

#include <switch.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "romfill.h"

extern char __sonadv_romdata_start[];

#define ROM_DIR "sdmc:/switch/SonAdvance-nx"
#define ROM_MAX_SIZE (32u * 1024u * 1024u)
#define HDR_SIZE 92

enum { OP_COPY = 0, OP_LIT = 1, OP_SKIP = 2 };

// Running CRC-32 (zlib's): start with 0xFFFFFFFF, finish with ~.
static u32 Crc32Update(u32 c, const u8 *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

static void *ReadAll(const char *path, size_t *outSize, size_t maxSize)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > maxSize) { fclose(f); return NULL; }
    void *buf = malloc((size_t)n);
    if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf != NULL) *outSize = (size_t)n;
    return buf;
}

static void Hex(char *out, const u8 *d, int n)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = h[d[i] >> 4]; out[i * 2 + 1] = h[d[i] & 15]; }
    out[n * 2] = 0;
}

int SonAdv_RomFill(SonAdvRomInfo *info)
{
    memset(info, 0, sizeof(*info));

    // 1. The table, from RomFS.
    size_t tabSize = 0;
    u8 *tab = NULL;
    if (R_SUCCEEDED(romfsInit())) {
        tab = ReadAll("romfs:/romfill.bin", &tabSize, 16u * 1024u * 1024u);
        romfsExit();
    }
    if (tab == NULL || tabSize < HDR_SIZE || memcmp(tab, "SAROMFL1", 8) != 0) {
        snprintf(info->error, sizeof(info->error), "The port's data table (romfs:/romfill.bin) is missing or damaged.\nPlease reinstall SonAdvance-nx.");
        free(tab);
        return -1;
    }
    u32 version, nEu, nUs, litSize, crcEu, crcUs;
    u64 anchor, dataSize;
    memcpy(&version, tab + 8, 4);
    memcpy(&anchor, tab + 12, 8);
    memcpy(&dataSize, tab + 20, 8);
    memcpy(&nEu, tab + 28, 4);
    memcpy(&nUs, tab + 32, 4);
    memcpy(&litSize, tab + 36, 4);
    memcpy(&crcEu, tab + 40, 4);
    memcpy(&crcUs, tab + 44, 4);
    const u8 *shaEu = tab + 52, *shaUs = tab + 72;
    const u8 *euOps = tab + HDR_SIZE;
    const u8 *usOps = euOps + (size_t)nEu * 16;
    const u8 *lit = usOps + (size_t)nUs * 20;
    if (version != 1 || (size_t)(lit - tab) + litSize != tabSize) {
        snprintf(info->error, sizeof(info->error), "The port's data table has the wrong size or version.\nPlease reinstall SonAdvance-nx.");
        free(tab);
        return -1;
    }

    // 2. The player's ROM: any .gba file in the app folder whose SHA-1 matches.
    DIR *dir = opendir(ROM_DIR);
    u8 *rom = NULL;
    size_t romSize = 0;
    int which = 0; // 1 Europe, 2 USA
    int seen = 0;
    char firstBad[64] = { 0 }, firstBadSha[41] = { 0 };
    if (dir != NULL) {
        struct dirent *de;
        while (which == 0 && (de = readdir(dir)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len < 5 || strcasecmp(de->d_name + len - 4, ".gba") != 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", ROM_DIR, de->d_name);
            size_t n = 0;
            u8 *buf = ReadAll(path, &n, ROM_MAX_SIZE);
            if (buf == NULL) continue;
            seen++;
            u8 d[SHA1_HASH_SIZE];
            sha1CalculateHash(d, buf, n);
            if (memcmp(d, shaEu, 20) == 0) which = 1;
            else if (memcmp(d, shaUs, 20) == 0) which = 2;
            if (which != 0) {
                rom = buf; romSize = n;
                snprintf(info->romName, sizeof(info->romName), "%s", de->d_name);
            } else {
                if (firstBad[0] == 0) {
                    snprintf(firstBad, sizeof(firstBad), "%s", de->d_name);
                    Hex(firstBadSha, d, 20);
                }
                free(buf);
            }
        }
        closedir(dir);
    }
    if (which == 0) {
        if (seen == 0) {
            snprintf(info->error, sizeof(info->error),
                     "No Sonic Advance ROM found.\n\nCopy your own Sonic Advance (GBA) dump into\n  " ROM_DIR "/\n"
                     "with a file name ending in .gba. Europe and USA dumps both work.");
        } else {
            snprintf(info->error, sizeof(info->error),
                     "%s is not a supported Sonic Advance dump.\nSHA-1 %s\n\nSupported (SHA-1):\n  Europe eb00f101af23d728075ac2117e27ecd8a4b4c3e9\n  USA    d842afa7dd1e84de08addb94a51506f1bcafd551",
                     firstBad, firstBadSha);
        }
        free(tab);
        return -1;
    }
    info->region = which;

    // 3. Fill.
    uintptr_t base = (uintptr_t)__sonadv_romdata_start - (uintptr_t)anchor;
    size_t copied = 0, litBytes = 0, skipped = 0;
    // The check covers exactly the bytes written, in table order: the section
    // also holds pointers relocated to a different address on every launch.
    u32 crcRun = 0xFFFFFFFFu;
    if (which == 1) {
        for (u32 i = 0; i < nEu; i++) {
            u64 dst; u32 src, len;
            memcpy(&dst, euOps + i * 16, 8);
            memcpy(&src, euOps + i * 16 + 8, 4);
            memcpy(&len, euOps + i * 16 + 12, 4);
            if ((size_t)src + len > romSize) goto bad_table;
            memcpy((void *)(base + dst), rom + src, len);
            crcRun = Crc32Update(crcRun, (const u8 *)(base + dst), len);
            copied += len;
        }
    } else {
        for (u32 i = 0; i < nUs; i++) {
            u32 type, src, len; u64 dst;
            memcpy(&type, usOps + i * 20, 4);
            memcpy(&dst, usOps + i * 20 + 4, 8);
            memcpy(&src, usOps + i * 20 + 12, 4);
            memcpy(&len, usOps + i * 20 + 16, 4);
            if (type == OP_COPY) {
                if ((size_t)src + len > romSize) goto bad_table;
                memcpy((void *)(base + dst), rom + src, len);
                crcRun = Crc32Update(crcRun, (const u8 *)(base + dst), len);
                copied += len;
            } else if (type == OP_LIT) {
                if ((size_t)src + len > litSize) goto bad_table;
                memcpy((void *)(base + dst), lit + src, len);
                crcRun = Crc32Update(crcRun, (const u8 *)(base + dst), len);
                litBytes += len;
            } else {
                skipped += len;
            }
        }
    }
    free(rom);

    // 4. Check: CRC of the filled data against the build tool's.
    (void)dataSize;
    u32 crc = ~crcRun;
    u32 want = (which == 1) ? crcEu : crcUs;
    info->copied = copied;
    info->literal = litBytes;
    info->skipped = skipped;
    info->crc = crc;
    info->crcOk = (crc == want);
    free(tab);
    if (!info->crcOk) {
        snprintf(info->error, sizeof(info->error), "Filling the game data from %s failed its check\n(CRC %08x, expected %08x).", info->romName, crc, want);
        return -1;
    }
    return 0;

bad_table:
    free(rom);
    free(tab);
    snprintf(info->error, sizeof(info->error), "The port's data table does not fit the ROM.\nPlease reinstall SonAdvance-nx.");
    return -1;
}
