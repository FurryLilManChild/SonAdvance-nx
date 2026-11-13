# SonAdvance-nx

Native Nintendo Switch port of Sonic Advance (GBA) from the SAT-R decomp. See `docs/design.md`, `docs/upstream.md`, `docs/progress-log.md`.

## Who does what

- Mark: tests on his Switch (unpatched V1, Atmosphere, sys-clk) and PC, runs PowerShell commands when asked, makes design decisions. Writes no code.
- Claude: all code, scripts, builds, log reading.

## Hard rules

- No em dashes anywhere (code comments, docs, commit messages, chat). No decorative emoji.
- Commit as `FurryLilManChild <346934328+FurryLilManChild@users.noreply.github.com>`.
- Never commit ROMs, extracted data or build output (see `.gitignore`); run `git status` before each commit.
- Before the title screen draws: cloud boot tests, fix and rebuild without asking. After that: present findings and the next build plan, wait for Mark's go-ahead.
- Switch test builds go to `C:\SonAdvance-nx\out\sd\switch\SonAdvance-nx\` on Mark's PC, replacing the previous .nro. Logs come back in `C:\SonAdvance-nx\out\sd\switch\switchlogs`.
- Never file issues or pull requests to SAT-R/sa2 (no-LLM policy).
- Mark's design choices are binding; raise any change of direction first.

## Layout

- `assets\` on Mark's PC: both dumps (Europe eb00f101..., USA d842afa7...). Not in git.
- `upstream/sa2`: SAT-R/sa2 submodule, pinned. Our changes to its files live in `switch/patches/`, applied by `switch/build.sh`.
- `switch/`: our Switch platform layer, build scripts, deko3d presentation, port menu.
- `docs/`: design, upstream, progress log.
