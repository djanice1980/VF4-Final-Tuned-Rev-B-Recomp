# VF4 Final Tuned recomp: design

A native Windows x86-64 build of *Virtua Fighter 4 Final Tuned (Ver. B)*
(Sega AM2, Naomi 2, 2004) that runs on Windows and under Steam/Proton. The
game's SH-4 program is translated to C ahead of time **on the player's own
machine, from the player's own MAME set**, and compiled together with a
runtime that does the work of the Naomi 2 hardware. No CPU emulation at play
time; the graphics are drawn on the PC's GPU.

The distribution model is Hookshot's (github.com/NotRexed/Hookshot): this
repository and its releases carry the translator, the runtime and a builder,
never the game, its code or an executable made from it. The builder reads the
player's set, checks it, translates, compiles and installs. A new release
means building again; saves and settings are kept.

This document is the plan. Sections marked **(decided)** were settled with
the maintainer on 2026-11-12; the rest are proposals to confirm as each
milestone starts.

## 1. What the work stands on

| From | What | Used how |
|---|---|---|
| `vf4ft-decomp` (public: djanice1980/VF4-Final-Tuned-Rev-B-Decomp-Source) | every one of the 6,632 routines of `main.bin`: entry, extent with literal pool, name, file; the FPU mode (SZ, PR) of every instruction; code tables; hand-written routines and their case tables | a pinned snapshot of the tables (addresses and names only, no code) in `meta/`, refreshed by a script; the translator works from these instead of finding code itself |
| `VF4debug` (private, MIT, same maintainer) | the SH-4 interpreter, the BIOS stand-in, protocol-level models of every device the game touches (system chip interrupts, Maple/JVS, cartridge port and DIMM, cards, network board), the reader of the ELAN and rendering-chip lists (`view.c`) | the device models and the BIOS stand-in are copied into `runtime/` and adapted to real time; the interpreter becomes the fallback for any code the translator did not cover and the reference that translated code is checked against |
| Hookshot | the user-side build model, the builder/launcher flow, update-by-rebuild | the model only; no code |

Rule carried over from both projects: **no code from emulators or SDKs**.
Public emulator sources (MAME, Flycast) and the Naomi homebrew SDK may be
read as references for hardware behavior, and are cited where they inform a
model; nothing is copied. The decomp's names are used as they are.

## 2. The pipeline

```
 gds-0036f.chd + vf4tuned.zip (317-0387-COM PIC)   the player's MAME set, read in place
        │  builder: CHD reader, GD-ROM file system, PIC key, DIMM DES (as the decomp's tools/extract.sh)
        ▼
 work/vf41_s.dec (240 MiB DIMM image)  →  main.bin (0x0C020000, 0x340000 bytes)
        │  checks: CHD SHA1 68edece4239e5adfef9df143defb711ff2b4db72,
        │          main.bin SHA1 e1288dda29d526c086086042b760fdc10be4662a
        ▼
 translator/ (Python 3)  +  meta/ (routine table, FPU context, code tables)
        │  one C function per routine, grouped into ~64 files (work/gen, local, never committed)
        ▼
 clang (llvm-mingw, pinned release) -O2, lld
        │  linked with the prebuilt runtime (runtime/, SDL3)
        ▼
 vf4ft.exe  +  SDL3.dll  +  data/vf41_s.dec  (the install folder; saves in user/)
```

The DIMM image is needed at run time as well: the game loads its assets from
it through the cartridge port model, exactly as on the board. `test.bin` (the
operator test mode program) is translated the same way and linked into the
same executable with its own dispatch table: it loads at the same address as
`main.bin`, so the runtime picks one program at boot, as the BIOS does.

## 3. Platform (decided: Windows exe that runs under Proton)

- **Target:** Windows 10/11 x86-64. Under Steam on Linux and Steam Deck, the
  same exe is added as a non-Steam game and run with Proton.
- **Compiler: llvm-mingw** (clang + lld + mingw-w64 headers). It is freely
  redistributable, so the builder can download it for the player, and it has
  Linux-hosted releases that produce the same Windows exe: a Linux or Steam
  Deck player builds natively and plays under Proton. MSVC / xwin (the
  Estacado toolchain) is not used, because its headers and CRT cannot be
  handed out by the builder.
- **Platform layer: SDL3**: window, keyboard, gamepads (Steam Input arrives
  as an SDL gamepad), audio output, and SDL_GPU for drawing (Direct3D 12 or
  Vulkan on Windows; under Proton, vkd3d-proton or Wine's Vulkan, with a
  setting to force one).
- A native Linux build is kept working for development (gdb, sanitizers,
  no Wine in the loop) and is also a release (section 11).

## 4. Translation

### Routine boundaries and control flow

Every routine comes from `meta/`: entry, extent, literal pool. The translator
never guesses where code starts. Inside a routine it decodes from the entry,
follows branches and delay slots, and emits C with labels and `goto`.

- **Delay slots**: the slot instruction is emitted before the branch, with
  the branch condition and target computed first where the slot changes them.
- **Calls** (`jsr`, `bsr`, `bsrf`): a direct C call when the target is a
  known routine entry; otherwise through the dispatch table (guest address →
  host function, sorted array, looked up by binary search; cached per call
  site).
- **Returns**: `rts` is a C `return`. A routine is entered with the guest PR
  set by the caller, as on the hardware, so code that reads or saves PR sees
  the right value.
- **Computed jumps inside a routine** (`braf` case tables, `jmp @rN` tails):
  the translator resolves GCC's case tables (byte or word offsets after a
  `mova`, bounded by the `cmp/hi`/`cmp/hs` on every way in, following copies
  of the index and values a `cmp/eq` decided), switches on a constant, and
  `mov`/`add`/`and`/`or` constants into `jsr`/`jmp`. What it cannot see
  through is listed by hand in `translator/hints.tsv`, each row with its
  reason (M1: the clipper's 3-bit `braf r9` ladders and one table of code
  addresses). A jump left unresolved goes through `rt_jump`, which only
  allows the start of a routine (a tail call).
- **Uncached aliases**: code jumped to through P1/P2 (`0xac...`, the cache
  routines run themselves uncached) is the same code at its P0 address.
- **Missing entries**: a call to an address no listed routine starts at
  makes it a routine of its own (`sub_<address>`; six newlib routines of
  `test.bin` the decomp's list leaves out).
- **Decided branches**: `bf/s x; slot; bt y` never falls out of the `bt`;
  the translator drops such impossible ways (they led into data).
- **Tail calls** into another routine (`jmp`/`bra` out of the extent) become
  a call followed by a return.
- **Interpreter fallback.** Anything the translator does not cover (an entry
  in the middle of a routine, code that is not in `main.bin`, an unresolved
  computed jump) runs on the interpreter until it returns to a known routine
  entry. In a correct build this never happens after boot; the log says when
  it does.

### Registers, flags and the FPU

- Guest state is one struct (`r[16]`, the two banks, `sr` with T split out,
  `gbr`, `mach/macl`, `pr`, `fpscr`, `fpul`, `fr[32]` as both banks). Locals
  are cached in C variables inside a routine and written back at calls and
  exits; the compiler does the rest.
- **FPU mode is static where flow decides it.** Each routine gets a summary
  of what it leaves in SZ and PR in terms of what it found ("as on entry",
  "flipped", 0, 1, unknown), worked out to a fixed point over the whole call
  graph; entry modes then come from the agreement of all call sites, with
  routines nothing calls directly starting in GCC's mode (0, 0). FPSCR
  loads from constants (including `lds.l @rN+` from a literal) and
  save/restore pairs are followed. The modes are checked against
  `meta/main/fpu_context.tsv`: 207 agree, 0 disagree. Instructions whose mode
  stays unknown (64, in the two hand-written stack-switch routines) test FPSCR
  at run time and carry both forms.
- Floating-point details follow the interpreter's documented behavior (NaN
  pattern, flush-to-zero, `fsca` and `fsrra` tables, `fipr`/`ftrv` component
  order), with shared code so the two always agree.

### Memory

- A 4 GiB reservation is the guest address space. RAM, video memory and
  the sound memory are mapped where the hardware puts them, with the
  P0/P1/P2/P3 mirrors made as extra views of the same memory
  (`VirtualAlloc2`/`MapViewOfFile3` placeholders on Windows, `mmap` on
  Linux), so a translated load is `*(T *)(base + addr)`.
- Pages that hold device registers, the store queues and the on-chip
  registers (P4) are left reserved. First version: every load and store goes
  through a 64 KiB page table (`mem_page[addr >> 16]`, as VF4debug does),
  with a slow path into the device models. Later, if profiling asks for it,
  plain RAM accesses whose address is provably in RAM skip the table.
- Endianness matches (both little-endian), so no byte swapping.

### Interrupts and timing

- The game runs on interrupts (vertical blank, render end, DMA end, Maple,
  DIMM). Translated code polls a single pending flag at loop back edges and
  routine entries. When an interrupt is pending and SR allows it, the poll
  does what the hardware does at an exception (SPC, SSR, SR.BL/MD/RB, the
  register bank) and calls the handler at VBR as a translated routine; its
  `rte` returns to the poll.
- **Real time instead of virtual time.** VF4debug moves a virtual clock;
  here the clock is the host's. The 3D library's empty wait routine
  (`busy_wait_hook_noop`) and the stopwatch wait at the end of a frame become
  host waits for the next device event. The vertical blank comes from the
  host's frame timing (60 Hz, the Naomi's 59.94 by default and selectable).
- A first search (2026-11-12) found no context switching and no code
  generated at run time; M1 checks this properly. If either turns up, the
  interpreter fallback covers it.

## 5. Runtime devices

Copied from VF4debug and adapted; each keeps its documentation section.

| Device | Source | Change for play |
|---|---|---|
| BIOS stand-in, system chip interrupts, timers | `hle_boot.c`, `hw.c` | real-time clock; the time-jump hooks become waits |
| Maple bus, JVS I/O board | `hw_maple.c` | fed from SDL: stick, Punch/Kick/Guard, Start, Coin, Test, Service; keyboard and gamepad mapping, rebindable |
| Cartridge port, DIMM image | `hw_cart.c` | reads `data/vf41_s.dec`; reads on a worker thread so a load does not stall the frame |
| EEPROM, backup RAM | `hw.c` | kept in `user/` |
| Card readers | `hw_card.c` | cards as files in `user/cards/`, inserted from a menu |
| Network board | `hw_dimm_net.c` | present but unplugged: the game takes its offline path. The scripted server stand-in stays a development tool |
| Graphics | `hw_gfx.c` + new renderer | section 6 |
| Sound | new | section 7 |

## 6. Graphics

The game sends display lists to the ELAN (Naomi 2's geometry chip) and
polygon/sprite blocks to the two rendering chips. VF4debug's `view.c` already
reads all of it (models, lights, matrices, fog, modifier volumes,
punch-through, translucency, render-to-texture) and draws it in software. The
renderer keeps that reading and replaces the rasterizer:

- The ELAN's transform and lighting stay on the CPU first (as in `view.c`),
  producing vertex buffers per scene. Moving them to shaders is a later
  optimization, decided by profiling.
- SDL_GPU draws: opaque, punch-through (alpha test), modifier volumes as
  stencil (the shadows), translucent sorted back to front per polygon as
  `view.c` does (order-independent transparency only if sorting proves
  wrong somewhere).
- Textures are decoded from video memory (twiddled, VQ, paletted, mipmaps)
  into a cache keyed by address, format and a content hash, invalidated on
  writes to video memory.
- Render-to-texture passes stay on the GPU as textures keyed by their video
  memory address; they are written back to guest memory only if the game is
  ever seen reading them (it is not, per VF4debug).
- Output: 640x480 4:3 by default, internal resolution scaling, integer or
  smooth scaling to the window, fullscreen. Widescreen needs the game's
  projection and HUD layout changed, so it is a later, separate piece of work.

## 7. Sound

The AICA sound chip runs Sega's sound driver (`aicadrv.bin`, loaded by the
game) on its own ARM7 core and mixes 64 channels with a DSP for reverb.

- ARM7 core: an interpreter (the driver is small and runs at a low clock), on
  the audio thread, stepped in time with the samples.
- Channels: PCM 8/16-bit and Yamaha ADPCM, loops, the envelope generator,
  pitch (rates as the decomp found them), pan and send levels.
- The DSP (reverb) last, from public descriptions of the chip; until then
  the effect sends are dropped.
- Output through SDL3 audio at 44.1 kHz.

## 8. Builder and launcher

Command line first, a window later:

1. `python -I tools/make_game.py --set <folder with gds-0036f.chd and vf4tuned.zip> --out <install folder>`:
   checks the set, extracts, translates, compiles, installs. `--update
   <install folder>` rebuilds after a new release, keeping `user/`.
2. Then a small builder program (Windows exe, and a Linux build for Steam
   Deck / desktop Linux): finds or asks for the set, downloads the pinned
   llvm-mingw and an embeddable Python, runs step 1, adds the game to Steam
   (section 11), and acts as the launcher with
   **Play** and **Update**.

The set is read in place and never changed. The install folder holds only
what the player's own set produced; the builder says so, and the README tells
players never to share it.

## 9. Checking that it is right

- **Instruction tests** (`tests/lockstep`, built `-m32` next to VF4debug's
  core, read from its checkout): every ordinary instruction word (56,294
  cases from 49,478 words, each FPU mode static and at run time) is emitted
  by the translator and run from random states against one step of
  VF4debug's interpreter, comparing every register, the store queues,
  instruction counts and the memory it addresses; FPU traps are compared
  too. Then routines of `main.bin` (VF4debug's pure routines and
  `tests/lockstep/routines.txt`) are called both ways, comparing the state
  and all of RAM. A broken instruction is caught on its first state (checked
  by planting one).
- **Lockstep against the interpreter**: a development mode runs the game in
  the translated build and records each routine entry and exit with the
  registers; the same recording from VF4debug on the same inputs must be
  identical for a boot to attract, a CPU vs CPU match and the 158 scenarios.
  Real-time waits are switched off in this mode (virtual time, as VF4debug).
- **Pictures**: the GPU renderer's frames against `view.c`'s at the same
  moments, compared at 1:1.
- **Under Proton**: every milestone that changes the window, input, audio or
  file paths is run under Proton (the Steam runtime and umu) on this machine
  before it is called done.

## 10. Milestones

| | What | Done when |
|---|---|---|
| M0 | Repository, llvm-mingw cross build from CachyOS, SDL3, an empty window exe | the exe opens a window and reads a gamepad under Proton |
| M1 | Translator for all of `main.bin` and `test.bin` | every routine translates and compiles with no warnings; instruction tests pass (done 2026-11-12, section 12) |
| M2 | Runtime with the device models, headless | boots to attract and plays CPU vs CPU matches; lockstep recording identical to VF4debug's |
| M3 | Picture, input, real time | playable at 60 fps in a window, silent, on Windows and under Proton |
| M4 | Sound | music, voices and effects; then the DSP |
| M5 | Builder and launcher | a fresh machine goes from the MAME set to Play with no other installs |
| M6 | Polish | resolution scaling, settings menu, cards, free play / arcade coin setting, test mode; widescreen considered |

## 11. Decided 2026-11-12

- **Native Linux is a release target too**, beside the Windows exe (which
  stays the one for Proton). Same runtime, built with the system's clang and
  SDL3.
- **Free play by default, arcade coins as an option.** A setting (saved in
  `user/`) switches between free play and "arcade" (coins from a key or
  button, with the game's own operator coin settings). It is applied by
  writing the game's own operator settings in backup memory, not by changing
  game code, so arcade mode behaves exactly as the board does.
- **The builder adds the game to Steam itself**: it writes a non-Steam
  shortcut to each Steam user's `shortcuts.vdf` (with Steam closed, after
  asking), and on Linux sets Proton as its compatibility tool for the Windows
  exe, or no tool for the Linux build.

## 12. M1 results (2026-11-12)

- `main.bin`: 6,632 routines, 432,307 instructions; 16,797 static calls,
  3,844 through registers; 313 computed jumps resolved (11 by hint), 4 left
  to run time: two exception-vector relays (`context_save_dispatch*`), a hook
  pointer (`vertex_colour_group_send`) and a jump into the boot ROM
  (`int_break_handler`). No illegal instruction reached, no bad delay slot.
- `test.bin`: 2,120 routines (6 found as call targets), 133,930
  instructions; 83 jumps resolved (10 by hint), 3 left to run time (the
  same relays and hook).
- The generated C (76 files, about 40 MB) compiles with no warnings under
  clang (Linux), llvm-mingw (Windows) and gcc `-m32` (the lockstep build),
  with `-Werror` on generated code in CMake (`vf4ft_gen_main`,
  `vf4ft_gen_test`).
- Lockstep: 1,804,564 checks, 0 failed (56,294 instruction cases x 32
  states; 3,156 routine runs over 50 routines). The routine runs reach 53
  routines: whole-program agreement is M2's lockstep recording.
- Known limits carried into M2: an FPU exception that traps in the middle of
  a routine does not stop the routine's C code (the game runs with no
  enables and DN set, so none should); `rt_jump` only reaches routine starts;
  interrupts are not modelled yet (`rt_event` is the host's).
