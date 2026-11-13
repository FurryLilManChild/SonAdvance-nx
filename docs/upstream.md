# Upstream projects

| Project | Pinned commit | License | What we take |
| --- | --- | --- | --- |
| SAT-R/sa2 (Sonic Advance Trilogy decomp) | 393357f7c (Jul 21 2026) | none posted | SA1 game code, data, build tools, gpsp software renderer, m4a audio mixer |
| boraeskicioglu/sonic_advance2_nx | 052423b6 (v0.1, Sep 3 2026; built from SAT-R/sa2 5efb59352) | MIT for its own Switch files (`LICENSE-PORT.md`) | Makefile `switch` target, AArch64 data alignment and relocatable music tables, flash save layer, 64-bit checksum fix idea |

Notes:
- SAT-R/sa2 has a strict no-LLM policy for issues and pull requests. We never file issues or send pull requests to it, and players report problems to this repo.
- No license on SAT-R/sa2: Mark has decided that is acceptable for his ports.
