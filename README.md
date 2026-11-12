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

## License

MIT, for this project's own work: see [LICENSE](LICENSE). The game and
everything made from it belong to Sega.
