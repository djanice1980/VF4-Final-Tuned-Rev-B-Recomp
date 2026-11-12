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
- A native Linux build of the runtime is kept working for development (gdb,
  sanitizers, no Wine in the loop). It is not a release target unless the
  maintainer decides otherwise.

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
  the translator resolves GCC's switch idiom and the hand-written tables the
  decomp documents, and emits a C `switch`. An unresolved target at run time
  goes to the interpreter fallback (below) and is logged, so it can be added.
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
- **FPU mode is static.** The decomp settled the SZ and PR mode of every
  instruction: the translator tracks it through each routine as the decomp's
  `AssignFpuContext` pass does (`fschg`, `frchg`, writes to FPSCR, the
  delay-slot rule) and takes `meta/fpu_context.tsv` for the places where flow
  alone gets it wrong, so each `fmov`/`fadd`/... is emitted for its known
  width. A run-time check in debug builds compares FPSCR against the expected
  mode at every routine entry.
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
   llvm-mingw and an embeddable Python, runs step 1, offers to add the game
   to Steam as a non-Steam game with Proton, and acts as the launcher with
   **Play** and **Update**.

The set is read in place and never changed. The install folder holds only
what the player's own set produced; the builder says so, and the README tells
players never to share it.

## 9. Checking that it is right

- **Instruction tests**: VF4debug's interpreter checks (1.42 M cases) are run
  through the translator as well: each case is translated, run, and its
  state compared.
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
| M1 | Translator for all of `main.bin` and `test.bin` | every routine translates and compiles with no warnings; instruction tests pass |
| M2 | Runtime with the device models, headless | boots to attract and plays CPU vs CPU matches; lockstep recording identical to VF4debug's |
| M3 | Picture, input, real time | playable at 60 fps in a window, silent, on Windows and under Proton |
| M4 | Sound | music, voices and effects; then the DSP |
| M5 | Builder and launcher | a fresh machine goes from the MAME set to Play with no other installs |
| M6 | Polish | resolution scaling, settings menu, cards, free play, test mode; widescreen considered |

## 11. Open questions for the maintainer

- Whether a native Linux build should ever be a release (it is kept working
  for development either way).
- Coin and free play: insert coins from a key/button, or default the
  operator settings to free play.
- Whether the builder should add the Steam shortcut itself (writing Steam's
  `shortcuts.vdf`) or show the steps.
