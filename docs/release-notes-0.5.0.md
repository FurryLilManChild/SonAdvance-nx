# SonAdvance-nx 0.5.0

A native Nintendo Switch port of Sonic Advance (Game Boy Advance), built from the SAT-R decompilation. The game runs as Switch code at 60 fps, with a widescreen view in the stages.

0.5.0 is feature-complete for this version but has not been played through to the end on the Switch yet.

## What you need

- A Switch that runs homebrew (Atmosphère), launched from the homebrew menu.
- Your own dump of Sonic Advance (GBA). **No game data is included.** The port reads everything from your dump when it starts. Both versions work:

| Version | SHA-1 |
| --- | --- |
| Europe | `eb00f101af23d728075ac2117e27ecd8a4b4c3e9` |
| USA | `d842afa7dd1e84de08addb94a51506f1bcafd551` |

## Install

1. Extract the zip to the root of your SD card. You get `switch/SonAdvance-nx/SonAdvance-nx.nro`.
2. Copy your dump into `switch/SonAdvance-nx/`. Any file name ending in `.gba` works. Keep only one `.gba` in that folder.
3. Launch "Sonic Advance NX" from the homebrew menu.

If the dump is missing or isn't one of the two above, the port shows a message saying what it found and what it expects.

## Features

- **Widescreen** in the stages, including re-laid-out act title cards and end-of-act results. Menus, the intro and special stages use the original 240x160 view.
- **60 fps** at stock Switch clocks.
- **Port menu** (press - and + together):
  - Restart the current act.
  - Jump into any of the seven special stages. Emeralds you win are kept, and you return to the first act of that zone afterwards.
  - Remap the controls.
  - Cheats: infinite lives, keep rings when hit, no damage, and all stages open in stage select (through X Zone and the Moon).
  - Exit the game.
- Saves and settings are kept in `switch/SonAdvance-nx/`.

## Known issues

- Tiny Chao Garden and link-cable multiplayer are not supported. Both need a second console linked by cable.
- With a USA dump the game runs on the Europe version's code, filled with the USA dump's data. The two versions play the same. The language screen is the Europe version's.
- Not yet played through to the end on the Switch.

## Fixed during development

- Special stages: the half-pipe drew as scattered pieces, the "GET RINGS" banner was garbled, rings showed as streaks, and the ring counter counted each ring several times. All four came from the port's map layout or from errors in the decompiled code, checked against the original game code.
- First boss: the hammer was missing.
- Extra Zone (the Moon): doubled sprites, ground seams and the laser's graphics.
- Exiting from the port menu no longer shows an error.

## Credits

- SAT-R and contributors for the Sonic Advance decompilation (github.com/SAT-R/sa2).
- boraeskicioglu for sonic_advance2_nx, the Sonic Advance 2 Switch port, whose Switch build setup and save layer this port builds on.
- Port by FurryLilManChild.

Sonic Advance is a trademark of SEGA. This project is not affiliated with SEGA or Nintendo.
