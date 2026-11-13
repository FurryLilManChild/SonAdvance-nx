# Progress log

Newest first.

## 2026-11-13: kickoff

- Read SAT-R/sa2 (pinned 393357f7c) and boraeskicioglu/sonic_advance2_nx v0.1 in full (the Switch diff is about 450 lines).
- Verified Mark's dumps: Europe `eb00f101af23d728075ac2117e27ecd8a4b4c3e9` (matches the decomp), USA `d842afa7dd1e84de08addb94a51506f1bcafd551`.
- Of 259 ROM-sourced data blobs in the SA1 build, 235 exist byte for byte in the USA dump; 24 differ (header, Tiny Chao Garden, address tables, extra-language text).
- Found two SA1 save problems in the SA2 port's code (64-bit checksum, no flush to SD except on exit). See design.md.
- Wrote design.md for review.
