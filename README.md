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

No game yet. The plan, with its milestones: [docs/design.md](docs/design.md).

| Milestone | State |
|---|---|
| M0: cross build, window, input, Steam/Proton | Done 2026-11-12. `vf4ft.exe` runs under Proton Experimental from Steam (Direct3D 12 through vkd3d-proton by default, Vulkan too) and natively on Linux; with Steam Input on it sees exactly one controller (Steam's virtual pad) and reads stick, D-pad and buttons |
| M1: translator | Done 2026-11-12. Both programs translate to C (6,632 + 2,120 routines) that compiles with no warnings for Windows and Linux; 1.8 million checks against VF4debug's interpreter, none failed ([details](docs/design.md#12-m1-results-2026-11-12)) |
| M2: runtime with the hardware models, headless | Next |

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

There is no game in it yet: the program is M0's platform layer alone, a
window drawn through SDL_GPU that reads keyboard and gamepads and writes
what it sees to `vf4ft.log` beside the program. The translated game code
(M1, next section) is built as libraries beside it.

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
   cd build/win64 && WINEPREFIX="$PWD/../proton-prefix" GAMEID=umu-default PROTONPATH="$HOME/.steam/steam/steamapps/common/Proton - Experimental" umu-run ./vf4ft.exe --frames 300
   ```

   Then read `build/win64/vf4ft.log`. Options: `--frames N` (stop after N
   frames), `--gpu direct3d12|vulkan`, `--log FILE`. Keys: arrows/WASD,
   J K L (Guard Punch Kick), Enter (Start), 5 (Coin). Quit with Esc, or
   hold View + Menu (Back + Start) on a controller for a second.

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

`meta/` holds the routine list and FPU modes taken from the decompilation
(addresses and names only). After the decompilation changes, refresh it with
`python3 -I tools/sync_meta.py ../vf4ft-decomp` and translate again.

## License

MIT, for this project's own work: see [LICENSE](LICENSE). The game and
everything made from it belong to Sega.
