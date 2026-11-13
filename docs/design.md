# SonAdvance-nx design

Native Nintendo Switch homebrew port of Sonic Advance (GBA, 2001, Dimps / Sonic Team), with widescreen, cheats and quality-of-life options. Status: draft for Mark's review, Nov 13 2026. No build work starts until this is agreed.

## Goal

The game runs as native AArch64 code: every game function is a real Switch function, game memory is plain Switch memory, and timing is one game frame per display refresh. There is no CPU emulation and no recompiler, because the game exists as decompiled C source (SAT-R's Sonic Advance Trilogy decomp, SA1 marked 100% decompiled).

The one part that is still emulation-style is graphics: the game writes GBA video registers, tile memory and sprite tables, and a software compositor (upstream's gpsp renderer) turns those into a picture on the CPU. At 426x240 2D this costs a few milliseconds a frame, so it is not a speed concern. Replacing it with a GPU compositor is an option for later (see open decisions), not part of phase 1.

## What is known about the game

- Code: the whole of SA1 is C in `src/game/sa1` of SAT-R/sa2 (190 files). About 160 functions build from C but do not yet match byte for byte on the GBA; on the Switch they compile from their C bodies like everything else.
- Data: most assets are extracted in the decomp (graphics, tilemaps, songs). About 0.67 MB still comes straight from the ROM at build time (259 `.incbin` blobs, including the Tiny Chao Garden image and some text and font tables).
- Frame rate: 60 fps target on GBA hardware (59.73 Hz). We run one game frame per Switch refresh (60 Hz, 0.45% faster than the GBA, not audible or visible).
- CPU load: trivial for the Switch. SAT-R's port already runs on PSP and PS2.

## ROMs (both supported)

| Dump | SHA-1 | Status |
| --- | --- | --- |
| Europe (En,Ja,Fr,De,Es) | eb00f101af23d728075ac2117e27ecd8a4b4c3e9 | matches the decomp, Mark has it |
| USA (En,Ja) | d842afa7dd1e84de08addb94a51506f1bcafd551 | Mark has it |

Per TCRF, the regions differ in language choices only (USA has English and Japanese, Europe adds Spanish, French and German); no gameplay or text differences are listed. A quick check of the ROM-sourced blobs: 235 of 259 appear byte for byte in the USA ROM at other offsets. The 24 that do not are the ROM header, the Tiny Chao Garden image, and small tables holding addresses (which move because the USA layout differs) and the extra-language text.

Plan: the game code is always the European build (it is the one the decomp verifies). The port detects which dump the player has by hash. With the USA dump, data is read from the USA offsets through a mapping table we generate once from both dumps, and the language menu offers English and Japanese only. Gameplay is identical either way.

## Pieces

| Piece | Plan | Borrowed from |
| --- | --- | --- |
| Game code | SA1 C code compiled for AArch64 with devkitA64 | SAT-R/sa2 (pinned) |
| Build system | Upstream Makefile plus a `switch` target | boraeskicioglu's sonic_advance2_nx Switch target |
| AArch64 data fixes | `.balign 8` for pointer data (AArch64 reads `.align 8` as 256 bytes) and music pointer tables in relocatable data | sonic_advance2_nx |
| Threads | One game thread (core 1), audio (core 0), presentation (core 2) | our usual layout |
| Frame pacing | Wait on the real vsync event (no spin loop), one game frame per refresh, frames drawn vs presented in the log | our ports (cv64-nx window code) |
| Graphics compositor | upstream gpsp software renderer, 426x240 | SAT-R/sa2 |
| Presentation | deko3d: the frame uploaded as a texture and drawn as one quad filling the screen at the correct aspect. Shaders compiled offline into the NRO | our ports, TriAevum-Switch notes |
| Audio | upstream m4a mixer, 48 kHz out through audout or SDL audio, with a steady buffer (no clearing the queue on backlog) | SAT-R/sa2 mixer |
| Input | libnx pad, per-button remap, Switch Online GBA layout as default | ours |
| Saves | 64 KiB flash image in RAM, written to the SD card whenever the game saves (not only on exit), delete then rename for FAT | sonic_advance2_nx flash layer, plus fixes below |
| Port menu | Minus + Plus (Plus is Start, Minus is Select in game): resume, cheats, controls, display, audio, quit | ours |
| Diagnostics | logs per stream with `.prev` copies, build stamp, crash handler, heartbeat, screen capture on a thumbstick click | ours |

## Known issues inherited from sonic_advance2_nx (to fix, not copy)

- SA1's save checksum adds memory in 8-byte steps on 64-bit (`uintptr_t`); the SA2 port fixed this only in SA2's save code. I think it makes SA1 reject its own save after a restart; the first save test will show it.
- SA1 never calls the save flush, so a save only reaches the SD card on a clean exit. Quitting through HOME would lose progress.
- The frame loop spins a CPU core at 100% between frames.
- Audio clears its queue when it backs up (a crackle risk).
- Presentation through SDL's renderer is OpenGL ES. We use deko3d instead.

## Widescreen

Upstream renders gameplay at 426x240 (a build-time setting, `WIDESCREEN_HACK`). Screens laid out for 240x160 (title, character select, zone map, menus, some cutscenes) end up offset or clipped; the SA2 port lists the same issues. sonic_advance2_nx turned off upstream's extended sprite table for SA1 because it dropped or misplaced HUD and menu sprites; we start from that finding.

Approach: fix each screen in the game code (centre or extend its layout to 426 wide), screen by screen from captures. Where a screen cannot sensibly be widened, show it at 240x160 centred with borders.

A 4:3 (original 240x160) option: the width is a compile-time constant throughout the game code, so a runtime switch is real work. Options: a second build of the same NRO code with the original size, or making the width a runtime value later. Not in the first phases.

## Cheats (port menu, all off by default)

Direct writes to game state, no codes needed: infinite lives, keep rings (no ring loss on hit), invincibility, unlock all characters (Amy included) and zones, stage select, and possibly Super Sonic in any stage. The exact list is checked against the game's code and manual before building.

## Quality of life ideas (for Mark to pick)

- Per-button remapping.
- Restart act from the pause menu.
- Saves written immediately.
- Language choice (all five with the Europe dump, two with the USA dump).

Not planned: multiplayer (link cable) and Tiny Chao Garden.

## Phases

1. Repo setup, build environment, first build. Cloud boot tests until the title screen draws; Claude builds and fixes without asking until then.
2. First Switch test (switch-test1): boot, title, first act, save, quit and relaunch.
3. Widescreen fixes for the 240x160 screens.
4. Cheats and quality-of-life options.
5. USA dump support and, if releasing publicly, loading all game data from the player's own dump at first start (below).
6. Coverage test against the manual's modes, then release.

## Public release and game data

A plain build links Sega's game data into the NRO: the extracted graphics and songs, and the 0.67 MB of ROM blobs. For a public release, the port would instead read data from the player's own dump at first start. Because the decomp rebuilds the European ROM exactly, every data symbol's position in that ROM is known from the GBA build's map file; the build records offset and size for each one, and the port fills them in from the dump (through the USA mapping table when needed). Tables made of pointers stay in the binary, since they are rebuilt as native 64-bit pointers on the Switch. This is phase 5 work, only needed for a public release.

## Decisions (Mark, Nov 13 2026)

1. Public release: yes. Phase 5 (game data read from the player's own dump) is in scope.
2. Quality of life: button remapping and restart act from pause, yes. Language choice: nice to have, low priority. Scaling options, LCD filter and skipping boot logos: no. Presentation is one fixed mode (fill the screen at the correct aspect).
3. GPU compositor: open to it later, after the core port works.
4. Version 0.1.0 to start; homebrew menu title "Sonic Advance NX".
5. Frame rate: 60 fps. The original already runs one full game update per refresh (one `VBlankIntrWait` per pass of `EngineMainLoop`), so the port matches it.
