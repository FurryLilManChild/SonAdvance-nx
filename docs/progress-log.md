# Progress log

Newest first.

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
