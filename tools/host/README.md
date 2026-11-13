# Host test harness

The Linux SDL build of the same game code (upstream `make sdl GAME_NAME=sa1`) is used for cloud checks of widescreen fixes.
Debug aid used there (not shipped): `BG3ONLY=1` hides background layers 0 to 2 and sprites in `VDraw`, so a stage's far background can be checked on its own.
