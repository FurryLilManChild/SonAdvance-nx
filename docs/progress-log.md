# Progress log

Newest first.

## 2026-11-19: switch-test10 built (instant replay)

- switch-test9 fix: The Moon's camera lower bound (pxHeight - 200) was past its upper bound for the 240-line picture, and the two clamps in camera.c alternated between them every frame (sprites 40 lines up, then down). Camera ranges smaller than the picture now settle the same way every frame (0018).
- Mark asked for the Sonic Rush style replay: left stick click saves the last 2 seconds (120 frames: drawn picture, OAM, I/O registers, palette, camera) to logs/replay_NNN_fFRAME.bin, at most 10 per session; tools/replay/replay.py turns one into PNGs plus a per-frame sprite list. Right stick click still saves a single BMP.

## 2026-11-13: switch-test6 built (replaces switch-test5, not yet tested)

- Super Sonic removed again before testing (Mark): a recolored Sonic with the invincibility effect is not what he wants, and a flying mode would need new code; leave it out. Lesson: ask before building a substitute for something Mark asked for.

- Mark: "All stages" must include the last level, and he wants Super Sonic in every act tried (I had dropped both on my own judgement; his call). Correction to the switch-test5 note: the course select's "FINAL ZONE" is X Zone (level 12) and was already listed; what was left out was the Extra Zone (The Moon, level 13).
- All stages (0016): course select count 15 for every character after the game's caps, so the list runs through Final Zone (X Zone) and Extra Zone (The Moon). The Moon is the game's Super Sonic flying stage; it forces Super Sonic whoever is picked (harness: all four characters load it).
- Super Sonic cheat (0016): SA1's own Super Sonic only flies (transform, move and dash sprites, final stage only). For normal acts: Sonic's sprites with Super Sonic's palette (sprite palette 11, same layout as Sonic's palette 0; the animation's palette load blocked with sprite flag 18), invincibility effect (enemies destroyed on touch, no hits, lava) without its music, speed shoes effect without the faster music, refreshed every stage frame. Sonic only; off in The Moon. Harness: gold Sonic, enemies destroyed on touch (score 200), a hit refused, much further along the act in the same time.

## 2026-11-13: switch-test4 and switch-test5 built

- switch-test3 report (Mark): everything good except "the software was closed because an error occurred" when exiting from the port menu. The log ends right after "exit:"; no crash report. Likely cause: our shutdown unmounted the SD card and closed the log, then newlib closed stderr (a file on the unmounted card), and the crash handler had no card to write to. switch-test4: exit through AM the way mp2-nx does (mode set at exit time, __libnx_exit ends the process if the applet teardown did not take over), card stays mounted, each exit step logged, crash handler opens the card itself. Mark: test it, but no more build rounds just for this.
- Phase 4 (switch-test5): port menu pages. Main: Resume, Restart act (single-player act only), Controls, Cheats, Exit game. Controls: each GBA button (A, B, L, R, Start, Select) on any Switch button, swap on conflict, reset. Cheats (0015, all off by default): infinite lives (the life is given back before the game takes it), keep rings when hit (knockback only), no damage (Coll_DamagePlayer refuses for the player; pits, lava, crushing and drowning still kill), all zones in course select (count raised to every zone up to Final Zone; the save is not changed; X Zone still needs the emeralds). Settings saved to settings.ini next to the save.
- Restart act: the game's own life-lost teardown, then GameStageStart (start of the act, no checkpoint, 0 rings, timer reset, title card again). No life lost.
- Checked in the host harness (new RESTART_AT, CHEATS, HIT_AT, KILL_AT, COURSE_AT, CHARSEL_AT, STATUS): restart replays the title card with rings and timer at 0; a hit keeps 8 rings with keep rings, is refused with no damage, drops all rings without cheats; a lost life leaves 3 lives with infinite lives, 2 without; course select lists Zone 1 Act 1 through Final Zone. The port menu itself (drawing and buttons) runs only on the Switch.
- Character select shows Amy even with an empty save in the harness; nothing to unlock there. Language: the game's own Options menu has the setting.

## 2026-11-13: switch-test3 built

- Mark's switch-test2 report: port menu text mirrored (wrong bit order in the font reader), game screen glitching behind the menu and a crash at RunDMAs (the menu re-ran the game's draw with its HBlank DMAs every frame; now a snapshot taken when the menu opens), crash on exit (applet exit mode set to 1; both logged sessions reached a clean exit, waiting on what Mark saw).
- Saves were never written: the decomp's PC flash identify (v126 flash) reported "no flash", so the game skipped saving. Fixed (0009); harness shows the save written.
- Language screen text missing in the wide build (coordinate type widened to 16 bits against 8-bit ROM data, 0010). SEGA logo garbage after it: the PC path of the BG map fill was wrong; native version from the ARM (0011).
- Act title card laid out for widescreen (Mark chose a full re-layout, 0012/0013): right-hand pieces on the right edge, everything but the banner on the bottom edge as on the GBA, full-width strip and zone name, sprite cells past the screen edge skipped instead of wrapping at x 512.
- End-of-act results laid out for widescreen (0014): strip and "GOT THROUGH" marquee full width and entering from past the right edge, bonus block centred, slide speeds scaled so every move takes the GBA's frame count. Two frame counts the decomp wrote as DISPLAY_WIDTH (240 frames) made the results last 186 frames longer in the wide build; restored to 240. Checked frame by frame against the 240x160 reference for act 1 and the act 2 exit.

## 2026-11-13: widescreen pass for switch-test2

- Host harness (tools/host): headless Linux build driven by a button script, frame captures, jump to any level or the special stage; plus a 240x160 reference build of the same tree. mGBA installed in the workspace for real-cartridge comparisons.
- Stage backgrounds: Neo Green Hill water extended below line 160 (0003). Angel Island 1/2, Cosmic Angel, X Zone: far background drawn 1.5x (gWideBgZoom, 0004/0005), showing exactly the GBA's 160 lines of art. Moon: surface extended with parallax (row map + 0006) and the Earth band offset so one Earth shows, centred (Mark: no second Earth).
- Menus, intro, title, character select, course select, special stage: their source files are compiled with the original 240x160 (include/sonadv_classic.h, 0007); the presenter shows that area centred at 1080x720 while no stage runs. Matches the 240x160 reference exactly.
- Found the decomp's PC build never draws "background sprites" (sub_80039E4 stubbed under PORTABLE, its C unfinished). Wrote SonAdv_WriteBgSpriteMaps from the original ARM (0008). Character select now shows the orange circles (matches the cartridge in mGBA); the Super Egg Robot's rotating body (BG2) now draws.
- Port menu (Minus + Plus): Resume, Exit game; game paused while open; + and - held back from the game until released. Applet focus and exit requests logged.
- Open: act title card is laid out for the left 240 pixels (Mark to choose a direction); special stage half-pipe looks wrong in both our build and the plain decomp build (to compare with the cartridge).

## 2026-11-13: switch-test1 built

- Cloud boot test: SAT-R's Linux SDL build of SA1 (same game code) reaches the intro, the title screen and the attract-mode demo. Widescreen artifacts on 240-wide screens (repeated art, stray tiles) as expected.
- Switch layer written (switch/platform/switch_main.c): deko3d presentation (game picture to RGBA8 texture, one quad, vsync paced by the swapchain), logs with .prev copies and build stamp, crash handler with module offsets, clocks in the log, SDL audio from a ring buffer (no queue clearing), save written to SD at the end of any frame where flash changed (temp file, delete, rename), right stick click capture.
- Build: devkitPro copied out of the danfromtico image into /opt/devkitpro (host tools need the workspace's newer glibc, so everything runs on the host). NRO 14.95 MB, sha256 04abfc86...dbbe05, build id "switch-test1 cddb44b".
- SA1 save checksum fixed to 4-byte steps (GBA-compatible). Earlier claim that the 8-byte version rejects the port's own saves was wrong: save and load use the same calculation; the real effect is that real-hardware or emulator saves would not load.

## 2026-11-13: kickoff

- Read SAT-R/sa2 (pinned 393357f7c) and boraeskicioglu/sonic_advance2_nx v0.1 in full (the Switch diff is about 450 lines).
- Verified Mark's dumps: Europe `eb00f101af23d728075ac2117e27ecd8a4b4c3e9` (matches the decomp), USA `d842afa7dd1e84de08addb94a51506f1bcafd551`.
- Of 259 ROM-sourced data blobs in the SA1 build, 235 exist byte for byte in the USA dump; 24 differ (header, Tiny Chao Garden, address tables, extra-language text).
- Found two SA1 save problems in the SA2 port's code (64-bit checksum, no flush to SD except on exit). See design.md.
- Wrote design.md for review.
