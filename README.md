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

Planning. Nothing runs yet. The plan, with its milestones:
[docs/design.md](docs/design.md).

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

There is no game in it yet: milestone M0 is the platform layer alone, a
window drawn through SDL_GPU that reads keyboard and gamepads and writes
what it sees to `vf4ft.log` beside the program.

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
   J K L (Guard Punch Kick), Enter (Start), 5 (Coin), Esc quits.

## License

MIT, for this project's own work: see [LICENSE](LICENSE). The game and
everything made from it belong to Sega.
