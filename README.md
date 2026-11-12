# VF4 Final Tuned (Ver. B): native recomp

*Virtua Fighter 4 Final Tuned (Ver. B)* (Sega AM2, Naomi 2, 2004) as a
native Windows program, for Windows and for Steam under Proton.

The game's SH-4 program is translated to C and compiled into a Windows
executable **on your PC, from your own MAME set**. The Naomi 2 hardware is
done by the program itself, and the pictures are drawn on your GPU. No
emulator runs at play time.

> [!IMPORTANT]
> This repository **does not contain the game, its code, its data or an
> executable**. You need your own MAME `vf4tuned` set: `gds-0036f.chd` and
> `vf4tuned.zip` (the 317-0387-COM PIC). Everything the build creates comes
> from your set and stays on your PC. Never share those files.

## Status

Playable in a window as a development build, with no sound yet (M3), from
the decompilation's files. The plan, with its milestones: [docs/design.md](docs/design.md).

| Milestone | State |
|---|---|
| M0: cross build, window, input, Steam/Proton | Done 2026-11-12. `vf4ft.exe` runs under Proton Experimental from Steam (Direct3D 12 through vkd3d-proton by default, Vulkan too) and natively on Linux; with Steam Input on it sees exactly one controller (Steam's virtual pad) and reads stick, D-pad and buttons |
| M1: translator | Done 2026-11-12. Both programs translate to C (6,632 + 2,120 routines) that compiles with no warnings for Windows and Linux; 1.8 million checks against VF4debug's interpreter, none failed ([details](docs/design.md#12-m1-results-2026-11-12)) |
| M2: runtime with the hardware models, headless | Done 2026-11-12. The translated game boots to attract and plays CPU vs CPU matches on VF4debug's hardware models, instruction for instruction the same run as VF4debug's (36,000 attract frames and two whole matches compared hash for hash); about 4 times the real hardware's speed with no picture yet ([details](docs/design.md#13-m2-results-2026-11-12)) |
| M3: picture, input, real time | Done 2026-11-12. The game runs in a window at 60 fps natively on Linux and under Proton (Direct3D 12), with keyboard and gamepads, and gives the same run as VF4debug's; played through coin, start and a match with a DualSense. The picture is VF4debug's software renderer, 8 to 9 ms a frame with the machine while playing ([details](docs/design.md#15-m3-results-2026-11-12)) |
| M4: sound | Next |

## How it works

1. The builder reads your MAME set, checks it is the supported version, and
   decrypts the program and the DIMM image.
2. The translator turns every one of the game's 6,632 routines into C. It
   uses the routine tables of the matching decompilation
   ([VF4-Final-Tuned-Rev-B-Decomp-Source](https://github.com/djanice1980/VF4-Final-Tuned-Rev-B-Decomp-Source)).
3. clang (llvm-mingw) compiles that C together with the runtime, which takes
   the place of the Naomi 2 hardware: inputs, the DIMM board, the ELAN and
   rendering chips (drawn with SDL_GPU), and the AICA sound chip.
4. You play the resulting `vf4ft.exe` on Windows, or add it to Steam as a
   non-Steam game and run it with Proton.

## Development build (CachyOS / Arch)

Steps 1 to 6 build the toolchain and the window. The game itself needs the
translated code (M1, "Translating the game") and the decompilation's files:
"Playing (development, M3)" below. Until the code is translated, only the
CPU runtime is built and there is no `vf4ft` program.

1. Tools. Each prints a version line:

   ```bash
   pacman -Q cmake ninja curl sdl3
   ```

   If any is missing:

   ```bash
   sudo pacman -S --needed cmake ninja curl sdl3
   ```

2. The Windows cross toolchain (llvm-mingw 20261104) and SDL3 3.4.20 built
   with it, into `toolchain/` (about 400 MB, SHA-256 checked):

   ```bash
   tools/fetch_toolchain.sh
   ```

3. Build the Windows program (`build/win64/vf4ft.exe` + `SDL3.dll`):

   ```bash
   cmake -S . -B build/win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build/win64
   ```

4. Build the native Linux program (`build/linux/vf4ft`):

   ```bash
   cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux
   ```

5. Run the Windows program under Proton without Steam. This needs
   `umu-launcher` (`sudo pacman -S umu-launcher`) and Steam's "Proton -
   Experimental" installed (Steam: Library, search "Proton Experimental",
   Install). The prefix is kept in `build/proton-prefix`:

   ```bash
   cd build/win64 && WINEPREFIX="$PWD/../proton-prefix" GAMEID=umu-default PROTONPATH="$HOME/.steam/steam/steamapps/common/Proton - Experimental" umu-run ./vf4ft.exe
   ```

   Then read `build/win64/vf4ft.log`. Controls and options: "Playing
   (development, M3)" below.

6. Run it from Steam (Steam Input). Close Steam first (Steam menu, Exit),
   then add the program as a non-Steam game set to Proton Experimental:

   ```bash
   python3 -I tools/steam_shortcut.py add build/win64/vf4ft.exe --name "VF4 Final Tuned (recomp test)"
   ```

   It backs up `shortcuts.vdf` and `config.vdf` beside themselves
   (`*.vf4ft-backup-<time>`) and changes nothing if it cannot read them
   exactly. `python3 -I tools/steam_shortcut.py list` shows the non-Steam
   games. Start Steam, launch the entry, and check `build/win64/vf4ft.log`:
   with Steam Input on, the pad reads "Steam virtual" (vendor 28de).

## Translating the game (development, M1)

The translator reads the two decrypted programs (`main.bin`, `test.bin`).
Until the builder exists (M5) they come from a
[decompilation](https://github.com/djanice1980/VF4-Final-Tuned-Rev-B-Decomp-Source)
checkout beside this one (`../vf4ft-decomp/work/`, made there from the MAME
set by its `tools/extract.sh`). Its output goes to `work/gen/`, which is
made from your copy of the game: never commit or share it.

1. Translate both programs (Python 3, no packages). Each run prints a
   summary and writes `work/gen/<program>/report.txt`:

   ```bash
   python3 -I translator/translate.py --program main --image ../vf4ft-decomp/work/main.bin --out work/gen/main
   ```

   ```bash
   python3 -I translator/translate.py --program test --image ../vf4ft-decomp/work/test.bin --out work/gen/test
   ```

2. Build as in steps 3 and 4 above: CMake picks up `work/gen/` and compiles
   it into `vf4ft_gen_main` and `vf4ft_gen_test` (warnings are errors).

3. The lockstep tests against VF4debug's interpreter. They need the
   [VF4debug](https://github.com/djanice1980/VF4debug) checkout at
   `../VF4debug` and gcc with 32-bit support; check with:

   ```bash
   pacman -Q gcc lib32-glibc lib32-gcc-libs
   ```

   If any is missing (multilib must be enabled in `/etc/pacman.conf`):

   ```bash
   sudo pacman -S --needed gcc lib32-glibc lib32-gcc-libs
   ```

   Then build and run (about 3 minutes; the last line is the verdict):

   ```bash
   make -C tests/lockstep -j16 test
   ```

## Playing (development, M3)

`build/linux/vf4ft` and `build/win64/vf4ft.exe` (steps 3 and 4, once
`work/gen/` exists) are the game in a window. Each reads `data/main.bin`
and `data/vf41_s.dec` beside itself. The build makes those as links to
`../vf4ft-decomp/work/` (or `-DVF4_DECOMP=<path>`), until the builder's
install layout (M5). Linux:

```bash
build/linux/vf4ft
```

Windows under Proton: step 5 above; from Steam: step 6. The start-up
before the warning screen (about 97 s of nothing on the real board) runs
as fast as it can, so the warning screen shows after 2 to 3 seconds. The
game runs at 60 frames a second from then on. It is Japanese settings with
coins: press Coin, then Start.

| | Keyboard (player 1) | Gamepad (first: player 1, second: player 2) |
|---|---|---|
| Move | arrows or WASD | left stick or D-pad |
| Guard / Punch / Kick | J / K / L | X / Y / B (West / North / East) |
| Punch + Guard (throw) | | A (South) |
| Start | Enter | Menu |
| Coin | 5 (slot 1), 6 (slot 2) | View |
| Test / Service | F2 / F3 | |
| Fullscreen | F11 or Alt+Enter | |
| Quit | Esc | hold View + Menu for a second |

Options: `--fullscreen`, `--gpu direct3d12|vulkan`, `--frames N` (quit
after N vertical blanks), `--unpaced` (no waiting, for measuring),
`--real-boot` (the start-up at its real pace too), `--image FILE`,
`--dimm FILE`, `--log FILE`. Every 600 frames `vf4ft.log` (beside the
program) records how long the machine took per frame, on average and at
most. No sound yet (M4). Rebinding, free play and settings are M6.

## Running the game headless (development, M2)

`build/linux/vf4ft_headless` (built by step 4 above once `work/gen/` exists)
is the translated game on VF4debug's hardware models, with no window. It
runs on virtual time and is driven by VF4debug's controller scripts. It
needs the [VF4debug](https://github.com/djanice1980/VF4debug) checkout at
`../VF4debug` (only its `tools/gen_tables.py` is run, at build time) and the
decompilation checkout at `../vf4ft-decomp` (the program image, the DIMM
image and the routine tables). Other places: `cmake -S . -B build/linux
-DVF4DEBUG_DIR=<path> -DVF4_DECOMP=<path>`.

1. Boot to the attract loop: 9,000 frames, then where the game is. The
   closing lines also count what ran on the interpreter instead of the
   translation:

   ```bash
   build/linux/vf4ft_headless --frames 9000
   ```

2. Draw what it shows into PNG files (`view/f<frame>.png`, every 600th
   frame):

   ```bash
   build/linux/vf4ft_headless --frames 9000 --view view --view-every 600
   ```

3. Check that a run is VF4debug's run. The script's `hash` lines fingerprint
   all of main RAM and the registers; this builds VF4debug's own binary
   from its checkout (into `build/vf4debug-ref`, needs gcc with 32-bit
   support as in "Translating the game" step 3), runs both on the script,
   and prints `same` or the first frame where they differ. A script that
   boots and hashes every 100 frames to frame 9000, then a check:

   ```bash
   python3 -c "open('build/every100.txt','w').write('run 100\nhash\n' * 90)"
   ```

   ```bash
   python3 -I tools/compare_runs.py build/every100.txt
   ```

   VF4debug's `scripts/` (a CPU vs CPU match, a two-player intrude match)
   work the same way once `hash` lines are added after their commands.

`--interp` runs the same binary on VF4debug's interpreter instead of the
translation (for comparisons; slower). `--help` lists the rest: they are
VF4debug's options.

`meta/` holds the routine list and FPU modes taken from the decompilation
(addresses and names only). After the decompilation changes, refresh it with
`python3 -I tools/sync_meta.py ../vf4ft-decomp` and translate again.

## License

MIT, for this project's own work: see [LICENSE](LICENSE). The game and
everything made from it belong to Sega.
