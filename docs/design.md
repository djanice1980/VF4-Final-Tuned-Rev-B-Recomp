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
- **Literal pool words the code writes** (an interrupt entry parks r0 in
  its own pool, `irq_entry_via_firmware`) are loaded from memory, not folded
  in as constants: every store whose address constant propagation knows
  marks the words it covers, to a fixed point with the propagation itself.

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
  DIMM). Translated code takes them **at the same instruction VF4debug's
  interpreter does** (decided in M2, below), so a run of the translation and
  a run of VF4debug on the same inputs are the same run, state for state.
- **Blocks.** Block starts are a property of the address, the same in every
  routine that holds the code (shared code included). Each block start
  checks once whether the next device event falls inside the block
  (`cycles + n > next_event`); if not, the block runs as C and adds its n
  instructions to the clock at once. If it does, the runtime runs that one
  block on the interpreter, which takes the event at its exact instruction,
  then the routine carries on in C from the block it reaches (every routine
  ends in a `switch` over its own block addresses for that).
- **Mid-block changes.** An access that leaves RAM (the slow path into the
  device models) is told its instruction's address and how far the block's
  count runs ahead of it, so device models see the exact clock and PC the
  interpreter would show them. A device can bring the next event forward
  (an interrupt raised by a write, an SR write unmasking one, a stop); the
  instructions that can do that check after themselves and finish the block
  on the interpreter when they did.
- **Exceptions nest.** When an interrupt or `trapa` is taken, the handler
  runs from VBR (the vector code is the BIOS stand-in's own few
  instructions, run by the interpreter) as a nested call: its `rte` returns
  to the instruction where it was taken, checked, and the C code carries on.
  The controller's stops (frame counts, `hash`, presses) are served the same
  way, in the middle of the C stack; only the end of a run unwinds it.
- **Real time instead of virtual time** (M3). VF4debug moves a virtual clock;
  here the clock is the host's. The 3D library's empty wait routine
  (`busy_wait_hook_noop`) and the stopwatch wait at the end of a frame become
  host waits for the next device event. The vertical blank comes from the
  host's frame timing (60 Hz, the Naomi's 59.94 by default and selectable).
- There is no context switching. There is code the game changes at run
  time (below).

### Code the game changes

M2 found one: at boot (vertical blank 9) `stage_screens_boot` calls
`code_jump_patch`, which copies a 12-byte jump stub over the start of the
3D library's `ta_input_finish`, so every later call goes to the game's own
`list_input_end_with_passes`. The translation of the original bytes would
still run. So the runtime watches for it in general:

- **Stores into code pages take a slow path.** Stores look up a second page
  table, `mem_wpage`, which leaves out the 64 KiB pages that hold translated
  code. Reads are unchanged. A store there that does not touch a translated
  halfword (the interrupt handler's own stack is inside `.text`, about 1,200
  stores a frame) is made at once. Every other store, from translated code,
  the interpreter, host code or DMA (`g_w*`), is compared with what is in
  memory first.
- **A changed block runs on the interpreter.** If a store changes a byte
  of a block, or of a pool word that a block compiled in as a constant (the
  translator lists those pairs: `M_lits`, about 49,000), that block is marked
  for good (`rt_code_changed`). Its `RT_BLOCK` check then sends it to the
  interpreter, and the routine carries on in C from the next block that is
  unchanged. The runtime also compares memory with the image file once at
  start, so changes made before the run (or by a snapshot) count too.
- **Targets taken from constants are checked.** A call or jump that the
  translator resolved from a constant register (`mov.l @(pool),r1; jsr
  @r1`) compares the register with that constant at run time and falls back
  to the run-time lookup if they differ. Code that set the register in
  another block may have been changed.
- **Not handled, and stopped with an error:** a store that changes
  instructions later in its own block. Also not covered: constant
  propagation's view of which accesses are plain RAM, which only decides
  where blocks check the clock, and the FPU modes found from pool words.
- Cost: about 1% for the per-block check (measured by removing it).

## 5. Runtime devices

Copied from VF4debug and adapted; each keeps its documentation section.
`runtime/machine/` is VF4debug's `src/` (all of it but `main.c`: the
interpreter, the memory map, the devices, the BIOS stand-in, the run loop,
the controller and its observer) as of the commit named in
`runtime/machine/ORIGIN.md`, changed only where marked `recomp:`, so a
later VF4debug fix can be carried over with a diff. M2 runs it in virtual
time, exactly as VF4debug does; the changes for play come in M3. Its routine
and global tables (names, addresses, argument shapes) are made at build time
by VF4debug's `tools/gen_tables.py` from the decompilation in development
builds; the player's build will make them from `meta/` instead (M5).

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

For later (added 2026-11-12, not yet read):

- Card readers: the arcade-projects.com thread on a card reader emulator for
  Naomi 2 / Chihiro / Triforce games (Initial D3, WMMT, Mario Kart, F-Zero AX),
  <https://www.arcade-projects.com/threads/naomi-2-chihiro-triforce-card-reader-emulator-initial-d3-wmmt-mario-kart-f-zero-ax.814/>.
  Worth reading when cards are done (M6), to compare against `hw_card.c`.
- Network: Flycast's VF.NET service, <https://vfnet.flyca.st/ui/>. Worth
  looking at as an online option in place of "unplugged". Before doing any of
  this, check what it expects from a client and whether it can be used.

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
- The DSP (reverb) last, from public descriptions of the chip (done:
  section 18). The game uses neither the filter nor the LFOs (section 17).
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
- **Lockstep against the interpreter**: `vf4ft_headless` (the translated
  game on VF4debug's machine, virtual time) runs the same controller scripts
  as VF4debug. Because interrupts and device accesses happen at the same
  instruction (section 4), the two runs are the same run. That is checked
  with the controller's `hash` (all of main RAM, the general registers, PC,
  PR and the instruction count) at fixed frames, compared with VF4debug's own
  binary built from its checkout. Where they part, snapshots (`save`,
  VF4debug's `tools/snapdiff.py`), the IRQ and IO order, and watchpoints
  find the first difference. `--interp` runs the same binary on the
  interpreter. Its runs agree with VF4debug's, but the reference is always
  VF4debug's own binary: an old output file once passed for one and hid a
  divergence for a while. Per-routine recordings were the first plan; hashes
  of the whole machine at every compared frame cover more.
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
| M2 | Runtime with the device models, headless | boots to attract and plays CPU vs CPU matches; lockstep recording identical to VF4debug's (section 13) |
| M3 | Picture, input, real time | playable at 60 fps in a window, silent, on Windows and under Proton (done 2026-11-12, section 15) |
| M4 | Sound | music, voices and effects (done 2026-11-12, section 17); the DSP's reverb (section 18) |
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
- The translator numbers above are M1's. M2 changed them: see section 13.

## 13. M2 results (2026-11-12)

- **Headless runtime**: `vf4ft_headless` (Linux) is the translated main
  program on `runtime/machine/` (VF4debug c3b2abb, with the `recomp:`
  changes listed in its `ORIGIN.md`) and `runtime/native/rt_native.c`, the
  seam that implements section 4's block protocol, nested exceptions and
  changed code.
- **Same run as VF4debug**, checked against VF4debug's own binary with
  `tools/compare_runs.py` (all of main RAM, registers, PC, PR and the
  instruction count at every hash):
  - boot and attract, hashed every 100 frames to frame 9,000: 90 of 90 the same;
  - attract for 36,000 frames (three rounds of title, character
    demonstrations and rankings), hashed every 1,000: 36 of 36 the same,
    33.3 billion instructions;
  - VF4debug's `scripts/cpu_vs_cpu.txt` (a whole match with the game's AI on
    both sides) with a hash after each command: 18 of 18 the same, to frame
    15,601;
  - VF4debug's `scripts/versus.txt` (a second player joins and the AI plays
    both sides of the two-player match): 29 of 29 the same, to frame 21,141.
- **Bugs this found in the translation**, all fixed:
  - code reached through the P2 alias (`0xac...`) has to run at its real PC,
    so it is left to run time;
  - `ldc rN,sr` needs a clock check after it, so every SR write gets one;
  - the control flow analysis kept call targets resolved in an earlier
    round (a stale `jsr @r1` target), so constant targets and decided
    branches are now checked again every round;
  - `ta_input_finish` is patched by the game at boot ("Code the game
    changes", section 4).
- **Translator now** (`main.bin`): 6,632 routines, 432,253 instructions;
  16,787 static calls, 3,853 through a register; 312 computed jumps
  resolved (11 by hint), 5 left to run time (`_start`, the two exception
  relays, the hook pointer, `int_break_handler`); 71,234 blocks; 701 pool
  words the program writes; 49,385 pool word readers (`M_lits`).
- **Speed** (virtual time, this machine, one core): 850 million guest
  instructions a second for the attract loop, about 4 times the real
  SH-4's 200 MHz. Per 9,000 frames, 17 million blocks have an event fall
  inside them and finish on the interpreter (92 million instructions).
  That is most of the remaining cost, and M3's work if real time needs it.
  VF4debug's interpreter runs the same loop at 215 million.
- **Lockstep tests** (`tests/lockstep`) on the new block protocol:
  1,804,564 checks, 0 failed.
- **Not done in M2**: the 158 VF4debug harness scenarios were not run
  against the translation. Many call routines with made-up arguments
  through the controller's `call`, which has not been tried on the native
  runtime. Real time, picture, input and sound are M3 and M4.

## 14. M3 plan (2026-11-12)

How the M2 machine becomes something you play, with as little change to the
machine as possible.

- **Time stays virtual and is paced at the vertical blank.** The machine runs
  exactly as in M2: cycles, events, the BIOS stand-in's time jumps. One
  `recomp:` hook in `machine.c`'s `vblank_event`, `machine_host_frame`, runs
  once per vertical blank on the machine's thread. It hands over the picture,
  applies the newest input, and waits until that frame is due on the host
  clock (60 frames a second, `FRAME_CYCLES`). If the run falls more than a
  quarter second behind (a long load), it drops the debt instead of
  catching up in a burst. The start-up before the warning screen
  (WARNING.INIT, 5,832 vertical blanks, about 97 s with nothing on screen)
  runs unpaced unless `--real-boot` is given. A frame takes about 1 ms of CPU in attract (section
  13) plus about 1.3 ms for the software picture, so there is room.
  As a result, a run depends only on the input set at each vertical blank.
  The same per-frame input gives the same run, so a play session can be
  recorded and checked against VF4debug like the M2 scripts.
- **Two threads.** The machine runs on its own thread (`rt_native_run`, no
  script). The main thread runs SDL: events, input and presenting. They
  share one mutex-guarded record: the newest picture with its frame number,
  and the input state. Quit sets `machine_interrupted`, which stops the run at
  the next vertical blank, and then the main thread joins the machine thread.
  Moving or resizing the window, or a slow present, does not stall the game.
- **Picture: `view.c` for now.** The software renderer draws every frame on
  the machine's thread with its own worker pool. A `recomp:` "host" mode turns
  it on with no files and no pipe. The hook copies the 640x480 picture
  (`shown`). The main thread uploads it to an SDL_GPU texture and blits it to
  the swapchain, scaled to the window at 4:3 with black bars
  (`SDL_BlitGPUTexture`, linear filter), so no shaders are needed yet. Vsync
  on; the window shows the newest frame at each refresh. The GPU renderer of
  section 6 replaces the software picture later; it does not block M3.
- **Input.** Keyboard and gamepads (Steam Input arrives as a gamepad) map onto
  `inputs` (`control.h`): player 1 on the keyboard and the first gamepad,
  player 2 on a second gamepad. Stick and D-pad give the directions. The face
  buttons give Guard, Punch and Kick, and South gives Punch + Guard (the
  throw). Start, Coin, Test and Service have keys (F2 Test and F3 Service).
  The table is in the README. A coin is the
  coin switch's rising edge, counted into `inputs.coins` as the controller
  script does. Free play and the saved setting are M6.
- **Windows.** The machine code builds with llvm-mingw:
  - `mem.c`: each RAM area is one `VirtualAlloc` with a spare page after it
    (the guard). The mirrors (P1, P2, aliases) live only in the page table,
    which the interpreter and the translated code both use. No code reads
    guest memory through host addresses equal to guest addresses.
  - `hw_cart.c`: a copy-on-write file mapping of the DIMM image
    (`PAGE_WRITECOPY`), the same as Linux's private `mmap`.
  - `view.c`: the CPU count from `GetSystemInfo`, and no pipe viewer on
    Windows. The worker pool keeps pthreads (llvm-mingw ships winpthreads).

  The routine and global tables (`tables.c`) are still generated from the
  decompilation on the build machine, as for the headless build. The player's
  build makes them from `meta/` in M5.
- **Programs.** `vf4ft` (the M0 window) becomes the game: the M0 host plus a
  machine thread. `vf4ft_headless` stays as it is for the comparisons. The
  game reads the image and the DIMM file from the same places as the headless
  build (`--decomp`, `--image`, `--dimm`, or the environment), until M5's
  install layout.

## 15. M3 results (2026-11-12)

- **Played** by hand on Linux with a DualSense: coin, start, character select
  and a match, with every button working; it felt right. The window
  showed every 600 frames in 10.0 s of real time and never fell behind.
- **Same run as VF4debug.** With no input, frame 9,000 has the same
  instruction count (4,431,349,708) in the window on Linux, in the Windows
  exe under Proton (Direct3D 12) and in the headless build. The M2
  comparisons against VF4debug's binary still match after the machine's M3
  changes: 90/90, 36/36, 18/18 (`cpu_vs_cpu`) and 29/29 (`versus`).
- **Cost per frame** (machine and software picture, on this machine: the
  view's pool caps at 8 threads): 8 to 9 ms on average while playing, 14.3
  ms at the worst, against 16.7 ms. The attract demo runs about 7 ms a
  frame, and the picture of a fight 5.5 ms (25,000 triangles). That leaves
  little room on slower machines such as the Steam Deck, so measure there
  before M4 adds sound. The GPU renderer (section 6) is the answer if it
  does not fit.
- **Not tried yet**: a second gamepad as player 2; the M3 exe started from
  Steam (M0's exe was, with Steam Input); Test and Service.


## 16. M4 plan (2026-11-12)

- **The real driver runs.** `aicadrv.bin` (the game loads it into sound RAM)
  runs on an ARM7 interpreter, `runtime/sound/arm7.c`, written from the ARM
  architecture manual. The driver is hand-written ARMv3 code: a main loop that
  polls without waiting, plus a timer A interrupt (FIQ) that the driver
  re-arms at 0xD4 for a sequencer tick every 44 samples. It reads the chip's
  monitor registers (play position, envelope level) to key off one-shots and
  free voices, so the chip model must answer them as the chip does.
- **The chip**, `runtime/sound/aica.c`, our own code from the chip's public
  descriptions and the decompilation's findings (`tools/dtpk_seq.py`,
  `tools/aica_adpcm.py`). It covers: 64 channels with PCM16, PCM8 and ADPCM;
  loops; pitch (OCT/FNS); the amplitude envelope with its rate tables; total
  level, direct level and pan; timers A to C; both interrupt controllers;
  the monitor registers; key on/off. The filter envelope and the LFOs come
  next, and the effect DSP (reverb) last. Until the DSP exists, the effect
  sends are dropped. (The game uses neither the filter nor the LFOs, so they
  were left out: section 17.)
- **Time.** The chip runs at 44,100 samples per virtual second on the
  machine's thread. An event every 32 samples (0.73 ms) runs, for each
  sample, the ARM for a fixed budget of instructions, then the timers,
  interrupts and mixer. The budget is a setting; real time is not involved,
  so the run stays deterministic and an audio hash can be checked. The main
  CPU sees sound RAM and the registers change at 32-sample steps.
- **VF4debug's mailbox model stays** for comparisons: `--sound mailbox`
  (the headless default, so `tools/compare_runs.py` still compares against
  VF4debug) or `--sound chip` (the game's default). With the chip, the game
  sees the real driver's answers: sequences are reported as playing and the
  handshakes come at the driver's own pace. That run is no longer
  VF4debug's, but it is still deterministic.
- **Output.** The mixer writes 16-bit stereo frames to a buffer. The game
  window hands each frame's samples (735 a frame) to an `SDL_AudioStream`
  at its vertical blank. The audio device's clock drifts from the host
  clock, so the stream's frequency ratio is nudged (at most 0.5%) to keep
  about 50 ms queued. During the unpaced boot the samples are dropped. The
  headless program writes them to a WAV file (`--wav FILE`) for checks.
- **Checks.** Every run with the chip is deterministic, which makes an audio
  hash possible. Music from the chip is compared against the decomp's
  renderer (`tools/dtpk_seq.py --wav`) for the same sequence. Then it gets
  listened to.


## 17. M4 results (2026-11-12)

- **Played and listened to** on Linux and from Steam under Proton: music,
  voices and effects, in time with the picture. The logs show the audio
  queue holding at about 50 ms, with only the unpaced start-up's samples
  dropped.
- **The real driver runs** on the ARM7 interpreter. Three points of the
  ARM rules matter to this driver, which calls with `stmfd sp!,{pc}`:
  - a stored PC is the instruction's address + 12;
  - an unaligned LDR rotates the word;
  - a FIQ's return address is the next instruction + 4.
  The ARM costs about 0.45 ms of host time a frame at 128 instructions a
  sample (`--arm-budget`).
- **The music matches the decomp's renderer.** The attract music
  (`bg_vf4ftadv1`) was compared against `tools/dtpk_seq.py --wav` with a
  spectrogram correlation: 0.80, against a baseline of 0.36.
- **Deterministic.** Two headless runs of the CPU vs CPU script with
  `--sound chip` give the same machine hashes and byte-identical WAV files
  (292 s, peak -2.2 dBFS, RMS -24 dBFS, nothing clipped). The headless
  default is still VF4debug's mailbox model, and its comparisons still
  match: 90/90 and 36/36 attract hashes, 18/18 `cpu_vs_cpu` and 29/29
  `versus`.
- **Mix level, an estimate.** The voice sum is scaled by 2^-17. At 2^-15,
  a fight clipped 103,000 samples. This was not checked against the
  board, since no recording from real hardware or Flycast was kept. Check it
  against a capture before M6.
- **Unused by VF4FT**, measured over a CPU vs CPU match (737 key-ons):
  - the filter: the driver sets LPOFF on every key-on, because no tone has
    filter data (tone word +0x10 & 0x7ff is 0);
  - the LFOs: no key-on has a depth.
  So both stay out of the model; the stats line reports any use.
- **The effect DSP is used**: 295 of the 737 key-ons send to it, and the
  game loads a DSP program. At first the sends were dropped (no reverb);
  section 18 adds the DSP.
- **The main CPU's interrupt** is wired: MCIPD & MCIEB to the system chip's
  external interrupt bit 1. The game enables the driver's doorbell (MCIEB
  0x020), but the driver never rang it in these runs.
- **The game's options:** the chip is the default; `--silent` gives the
  mailbox model (no sound, VF4debug's run).

## 18. The effect DSP (2026-11-12)

`runtime/sound/aica.c` runs the chip's effect DSP from the public
description of its registers and instruction word: 128 steps a sample,
TEMP (128 x 24 bits, addressed relative to MDEC_CT), MEMS, MIXS, EFREG,
the 26-bit accumulator, SHIFT with and without saturation, and the ring
buffer in sound RAM in the chip's 16-bit float format. No emulator's code
was used. The FPGA Saturn core's SCSP (the predecessor's DSP) is a
possible cross-check for the timing details, if they ever matter.

- **The game's program** was read out of the registers and disassembled.
  It is loaded once, at the start-up, and does not change. The game's own
  code settles the points the descriptions leave open:
  - every memory read and write is on an odd step;
  - each read is stored into MEMS by IWT exactly two steps later;
  - each output is written once a sample.
  The model follows that: a read lands two steps on, and what a step
  stores is the previous step's accumulator after SHIFT.
- **What it does.** Send 0 (MIXS0) feeds a stereo hall reverb: early
  reflections from about 100 ms, then a smooth decay of about 14 dB a
  second, with left and right decorrelated. Its outputs, EF0 (left) and
  EF1 (right), also carry the send itself at unity. Send 1 feeds a
  saturating "fuzz" (twelve saturating doublings, then x0.17) to EF2,
  centred. A CPU vs CPU match sends only to send 0. The ring buffer is 64K
  words at 0x7E0000.
- **Checked:**
  - the float format round-trips: every word packs back to itself, and
    every 24-bit value keeps its top 12 significant bits;
  - the program's impulse response, run offline from a register dump, is
    the reverb above, with nothing saturating;
  - a match's outputs peak at 9,967 of 32,767, with no saturated results;
  - two runs give byte-identical WAVs;
  - the machine's hashes are the same as without the DSP (its writes to
    sound RAM change nothing the game reads);
  - the mailbox comparisons against VF4debug still match.
  The DSP costs about 0.3 ms of host time a frame: its program is decoded
  when written, not every sample.
- **Listened to** in the game (2026-11-12): it sounds fine, and the reverb
  can be heard on the coin and start sounds, where silence follows a
  short sound. Under the music it blends in.
- **Level, an estimate.** A channel's send is scaled into MIXS so that a
  full-scale 16-bit sample is full scale for the DSP (MIXS's 20 bits are
  read into bits 23-4 of its 24-bit word). The game sends at IMXL 6
  against a direct level of 15: 27 dB down, at 3 dB a step. So the reverb
  is a quiet room under the dry sound, about 31 dB below it in a match.
  If the chip scales the send differently, the reverb is louder or
  quieter by the same factor; a capture from the board or Flycast of the
  same scene would settle it.
- **A word of 0 in the ring decodes to +0.5 full scale** (silence is
  0x6000). Until the DSP's own writes have swept the ring (1.5 s), parts
  of it read as a constant. The start-up's sound is dropped, so this is
  not heard.
