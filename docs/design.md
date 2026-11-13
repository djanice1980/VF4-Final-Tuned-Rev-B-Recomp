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
        │  builder (tools/vf4build): CHD reader, GD-ROM file system, PIC key, DES; no MAME tools
        ▼
 data/VF41_S.BIN (240 MiB, encrypted, as on the disc) + data/dimm.key  →  main.bin (decrypted in memory)
        │  checks: CHD SHA1 68edece4239e5adfef9df143defb711ff2b4db72, VF41_S.BIN SHA1,
        │          main.bin SHA1 e1288dda29d526c086086042b760fdc10be4662a
        ▼
 translator/ (Python 3)  +  meta/ (routine table, FPU context, code tables, globals)
        │  one C function per routine, grouped into ~64 files (<install>/build, removed afterwards)
        ▼
 clang (llvm-mingw, pinned release) -O3, lld; or the system's clang/gcc for native Linux
        │  with the runtime's sources (runtime/) and SDL3 (SDL's official mingw build)
        ▼
 vf4ft.exe  +  SDL3.dll  +  data/  (the install folder; saves in user/)
```

The DIMM image is needed at run time as well: the game loads its assets from
it through the cartridge port model, exactly as on the board. The install
keeps the disc's encrypted file and its key; the game decrypts it into
memory when it starts (0.17 s on 16 threads), as the DIMM board does when
it boots, and takes the program out of it. `test.bin` (the
operator test mode program) is translated the same way and linked into the
same executable with its own dispatch table: it loads at the same address as
`main.bin`, so the runtime switches between them at the BIOS's test-menu
service, as the BIOS does (section 20).

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
and global tables (names, addresses, argument shapes) are made at build time:
from `meta/` by `tools/make_tables.py` (the player's build, and CMake with
`-DVF4FT_TABLES=meta`), or by VF4debug's `tools/gen_tables.py` from the
decompilation (CMake's default when both checkouts are present), which adds
the declarations the observer's call log uses. Both give the same routines,
sizes and globals (checked: 6,632 and 4,266, identical).

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

Built in M6 (section 20). Two things differ from this plan: the textures
live in one storage buffer that the fragment shader reads itself, in place
of a cache of GPU textures, and the shaders are SPIR-V, so the device is
Vulkan (natively, or Proton's winevulkan).

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

Built in M5 (results: section 19).

1. `python -I tools/make_game.py --set <folder> --out <install folder>`:
   checks the set, extracts, translates, compiles, installs. `--update
   <install folder>` rebuilds after a new release from the install's own
   data (the set is not needed), keeping `user/`. `--target linux` makes a
   native Linux program with the system's compiler and SDL3.
2. `vf4ft-launcher` (`runtime/launcher/launcher.c`, SDL3 only): Choose set,
   Build, Update, Play, Add to Steam; it runs step 1 and
   `tools/steam_shortcut.py` and shows their output. The Windows release
   carries SDL's official SDL3.dll and Python's embeddable package
   (`python/`), so nothing is installed; the builder downloads the pinned
   llvm-mingw and SDL3 on its first run. On Linux the launcher uses the
   system's Python, and Play for the Windows exe goes through the Steam
   shortcut (Proton, Steam Input).
3. `tools/package_release.py` puts a release together from tracked files
   only, so nothing made from a set can get into one.

The set is read in place and never changed. The install folder holds only
what the player's own set produced; the builder's notice in it (`READ
ME.txt`) and the README tell players never to share it.

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
| M5 | Builder and launcher | a fresh machine goes from the MAME set to Play with no other installs (section 19) |
| M6 | Polish | resolution scaling, settings menu, cards, free play / arcade coin setting, test mode; widescreen considered (done 2026-11-12, section 20) |

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
- **The level, checked against the board (2026-11-12).** A channel's send
  is scaled into MIXS so that a full-scale 16-bit sample is full scale for
  the DSP (MIXS's 20 bits are read into bits 23-4 of its 24-bit word).
  The game sends at IMXL 6 against a direct level of 15: 27 dB down, at
  3 dB a step. So the reverb is a quiet room under the dry sound, about
  31 dB below it in a match.

  The reference is a playthrough captured from a real NAOMI 2 (YouTube
  ulNHQDq29Sk, "Virtua Fighter 4 Final Tuned Playthrough (Real Arcade PCB
  Capture)" by Yo-O, through a Micomsoft SC-580N1). It is an analog
  capture: its silences sit at -68 to -70 dBFS, not at digital zero. (The
  "By Urien84" longplays have digital-zero silences, so they are emulator
  captures.) The two runs never play the same thing at the same moment,
  so the check works sound by sound:
  1. A temporary build wrote our output as separate streams: the sounds
     that send to the DSP, everything else, the DSP's output, and MIXS0.
  2. 18 of the sounds that send were searched for in the capture by
     waveform correlation (200-4000 Hz). They were found at about 21
     places, with correlation 0.75 to 0.90; several of our sounds match
     the same places. The capture's polarity is inverted.
  3. Each sound's own reverb was made by running the game's program
     offline on that sound's MIXS0 alone, from an empty ring. Run on the
     whole MIXS0, the offline DSP reproduces the run's DSP output exactly.
  4. At each place, the capture was fitted as a x sound + b x its reverb
     (150-2500 Hz), with the capture's clock corrected.

  Averaged over the places in the capture, b/a = 1.0 +- 0.2. With
  our reverb shifted out of time by 0.75, 1.25 and 1.75 s, the same fit
  gives -0.06, 0.09 and 0.31: noise around zero. So the board's reverb
  matches ours in waveform, not only in level, and its level is ours to
  within about 2 dB. That checks the DSP model and the send scaling
  together.
- **The pitch, checked against the board.** The same attract music in both
  recordings, compared on a log-frequency spectrum, differs by 0.14 cents
  (80 parts per million): the capture's clock, not the chip model's pitch.
- **Why the first comparison failed.** Before the separate streams, the
  whole mix was compared, and that gave no result. The attract's pieces
  start up to a second apart on the board, the music and the effects
  shift separately, and the reverb rides on certain effects and voices,
  about 25 dB under music that is not aligned with them. The scratch
  tools are not in the repository; the steps above are enough to redo
  the check.
- **A word of 0 in the ring decodes to +0.5 full scale** (silence is
  0x6000). Until the DSP's own writes have swept the ring (1.5 s), parts
  of it read as a constant. The start-up's sound is dropped, so this is
  not heard.

## 19. M5 results (2026-11-12)

**What a player runs.** `vf4ft-launcher` (Choose set, Build, Update, Play,
Add to Steam) or `tools/make_game.py` directly. Section 8 has the flow and
README.md the steps.

**The set, with no MAME tools** (`tools/vf4build/`, each written from a
public description; no code from MAME, libchdr or the decompilation's
MAME-derived decrypter):

- `chd.py`: CHD v5. Header; the compressed hunk map (its Huffman code, the
  run lengths, copies from other hunks), checked against the map's CRC-16;
  CD hunks in LZMA, Deflate and FLAC (`flac.py`, RFC 9639), and Zstandard
  where the Python has it. The game's track 3 is LZMA, Deflate and copies;
  FLAC is only in tracks 1 and 2, but it was checked anyway: all 1,352 audio
  frames of track 2 match chdman's extraction, and 3,000 random sectors of
  track 3 match it too.
- `gdrom.py`: the ISO 9660 file system of the high-density area.
- `naomi.py`: the PIC's key and boot file name; BHX.BIN names VF41_S.BIN.
  The cipher is DES (FIPS 46-3, `des.py`, checked on the standard's worked
  example). Its byte order was found by trying the eight possibilities
  against the decompilation's decrypted image: the key is the PIC's eight
  key bytes reversed, and each block is the file's 8 bytes read as a
  little-endian number. Checks: the CHD's SHA-1, VF41_S.BIN's SHA-1 (as on
  the disc), main.bin's and test.bin's SHA-1.
- The whole step takes 5.5 s in Python, decryption of the two programs
  included.

**The install keeps the disc's file.** `data/VF41_S.BIN` (encrypted) and
`data/dimm.key`; the game decrypts the image into memory at start
(`runtime/host/dimm_image.c`, one thread per processor: 0.17 s for 240 MiB
here) and takes main.bin out of it, so no decrypted copy is ever written.
Its output is byte for byte the decompilation's `vf41_s.dec`.

**Tables from meta/.** `meta/main/routines.tsv` gained the routine sizes and
`meta/main/globals.tsv` the 4,266 named globals (`tools/sync_meta.py`,
names and addresses only). `tools/make_tables.py` makes the machine's tables
from them; they are identical to `gen_tables.py`'s in every routine,
size and global.

**Compiling with no CMake.** `tools/vf4build/build.py` runs the compiler
with CMake's flags, one process per processor. The compiler and SDL3 are
pinned by SHA-256 (`toolchain.py`): llvm-mingw 20261104 (the Linux or
Windows build of it) and SDL's official `SDL3-devel-3.4.20-mingw` package,
in place of the SDL3 the development build compiles itself. The
native Linux target uses the system's compiler and SDL3.

**Measured on this machine (32 threads):**

| Run | Time |
|---|---|
| `make_game.py`, empty download cache, Windows exe | 59 s (11 s downloading and unpacking, 35 s translating, 13 s compiling) |
| `--update` (no set) | 52 s |
| `--target linux` | 57 s |
| the Windows release under Proton, fresh prefix: launcher, embeddable Python, Windows llvm-mingw | 66 s |

**Checked:**

- The builder's `vf4ft.exe` under Proton (Direct3D 12) and natively boots
  from the encrypted image through the warning screen to the attract demo.
- In the Windows release under Proton, in a fresh prefix with nothing
  installed, the launcher was used by hand: Choose set (the Windows folder
  dialog), Build, Play. Its scripted mode (`--do build|update --exit`) ran
  Build and Update and showed the builder's output as it came.
- The first scripted Build, in the brand-new prefix, did not start; the same
  command worked twice afterwards. Found later (2026-11-12, after M6): the
  release zip's launcher was built at 19:08:58, before `--do` and `--exit`
  were added at 19:09:17, and `package_release.py` packaged
  `build/win64/vf4ft-launcher.exe` as it found it. That launcher has no
  `--do` or `--exit` in it at all, so it ignored both and waited for a hand
  on its buttons (which is how the hand-run Build and Play above were done,
  in that same window). The two runs that worked had a rebuilt launcher
  copied in. Not a Proton or first-run problem: the current release builds
  in a brand-new prefix with the same command (100 s with the machine
  loaded). Three things in our code let it pass in silence, now changed:
  `package_release.py` rebuilds the launcher in its CMake build folder
  first, and warns about builder files that are not committed; the launcher
  logs an option it does not know; and `--exit` ends with 1 when `--do`
  started nothing (it ended with 0, and `umu-run` passes the code on). The
  same look found `--do steam` pressing Settings (the action list and the
  buttons were in different orders); `--do` now names the buttons.
- Development builds are unchanged: CMake still makes the tables from the
  decompilation when both checkouts are there (`-DVF4FT_TABLES=meta` for the
  player's way), and the game falls back to `data/main.bin` and
  `data/vf41_s.dec`.

**Not done yet:**

- Releases: `tools/package_release.py` assembles the Windows release
  (launcher, SDL3.dll, the embeddable Python 3.14.10, the builder's tracked
  files: 15 MB zipped). No release has been published, and there is no CI
  yet.
- The Linux launcher links the system's SDL3. For Steam Deck it needs SDL3
  built in, or the Windows release run under Proton (the route M6 took,
  section 20).
- Add to Steam on Windows (registry SteamPath, `tasklist`) is written but
  not run on a real Windows Steam. On Linux it is the M0 tool.
- Play from the launcher on Linux, for the Windows exe, goes through the
  Steam shortcut, so Add to Steam has to come first. It was not run here,
  because Steam was open.

## 20. M6 results (2026-11-12)

Done: saves, free play or arcade coins, the card units, the settings file
with the launcher's Settings page (rebinding included), the GPU renderer
with resolution scaling, the Steam Deck route, and test mode (below).
Widescreen stays a separate piece of work (section 6).

**Saves.** `user/backup.bin` holds what the cabinet keeps with the power
off: the battery-backed RAM (64 KiB at 0xa0200000) and the settings EEPROM
(128 bytes), as one 65,664-byte file in VF4debug's backup-save layout. It
is loaded before the game starts, and written (to a temporary file, then
renamed) every 10 seconds when it has changed and when the game ends. A
file that is there but is not a backup is left alone: the run starts blank
and never writes over it.

**Free play, the operator's way.** The EEPROM's system area is two copies
of 18 bytes (at 0x00 and 0x12): a check word, five bytes of board settings
and eleven of coin settings. Before the game starts, the coin setting (byte
9 of a copy) is set to 26, which the BIOS stand-in's `coin_config_fix`
reads as free play. `coins = arcade` puts 0 (1 coin, 1 credit) back only
when free play is what it finds, so any other coin setting is kept. Both
copies are then made the same and sealed with the game's own check
(`crc16_ccitt`, the one `eeprom_check_copies` verifies at start). A blank
EEPROM is first given what the game writes on its first start. No game
code is changed. Checked headless: with free play Start goes to character
select with no coin; with the coin setting at 0, Start with no coin does
nothing and the attract demo goes on. In the window the attract screen
says FREE PLAY.

**Cards.** The cabinet's two card units are on by default (`cards = off`
removes them), with the cards in `user/cards/p1.card` and `p2.card` (128
bytes each). A card in the unit from the start works: the game reads it
at character select, writes it (five writes) and locks it.
A new card is kept once the game has written it; the window's card was the
harness's byte for byte. `hw_card_put` and `hw_card_get` are the change in
VF4debug's card model (`runtime/machine/ORIGIN.md`).

**Settings.** `user/settings.txt` (`runtime/host/settings.c`, shared by the
game and the launcher) is "name = value" lines: `coins` (free play |
arcade), `fullscreen` (auto | on | off; auto is on when Steam sets
`SteamDeck=1` or `SteamTenfoot=1`), `picture` (4:3 | stretch), `filter`
(smooth | sharp), `renderer` (gpu | software), `scale` (1 to 4), `sound`,
`cards`, and every control: `key1.*` and `key2.*` (keyboard, players 1 and
2) and `pad.*` (every gamepad; the first is player 1), up to four inputs
each, by SDL's key and gamepad names. Besides the game's three buttons
there are buttons for P+G (throw), P+K, P+K+G and K+G; the gamepad's
defaults are the PlayStation 2 versions' layout (section 21). A line the game does not
know is skipped with a message in `vf4ft.log`, so an old file or a hand
edit never stops the game. The launcher's Settings page writes the file:
one row per setting, and a Controls page per device where A takes the next
key or button, X clears, Y restores the default.

**The GPU renderer** (`runtime/host/gpu_render.c`, shaders in
`runtime/host/shaders`). `view.c` keeps reading the display lists,
transforming, lighting and sorting the translucent list (section 6); a pass
to the screen is now handed to the host as vertices, polygon states and
modifier volumes (`view_host_gpu`, the change to VF4debug's renderer in
`ORIGIN.md`) instead of going to the software rasterizer. The host draws it
with SDL_GPU by the rasterizer's rules:

- the background plane; then the opaque and punch-through lists with the
  chip's depth compares (depth is 1/w scaled by the scene's largest, the
  greater value nearer, cleared to 0);
- the opaque list's modifier volumes through the stencil: parity per
  volume, then inclusion or exclusion, then the shadow pass, which scales
  the colour of every polygon that takes shadows;
- the translucent list from the back, with the chip's blend factors.

The plan's texture cache became one storage buffer: an arena of 16 million
texels, of which only what changed is sent again. The fragment shader
fetches, wraps, mirrors and clamps texels itself, filters, picks the mip
level, and does bump maps, shading, offset colour, the alpha test and both
fogs. Colours are not perspective-corrected (as the chip draws them);
texture coordinates are. Passes into textures are still drawn by the CPU
into texture memory, as before. The shaders are GLSL compiled to SPIR-V by
`tools/make_shaders.py` (glslc), and the result, `gpu_shaders.h`, is
committed, so players need no shader compiler. SPIR-V means the device is
Vulkan: natively, or under Proton through winevulkan; when there is none,
or the renderer cannot start, the software picture is used as before.
`scale` draws at 1 to 4 times 640 x 480 (default 2, 1280 x 960).

Checked: at scale 1, on frames matched with `--shot` (the machine waits at
that vertical blank while the picture is saved), the GPU picture matches
the software one at 1:1 zoom: a mean difference of 1.6 to 2.4 (of 765),
in scattered single pixels. The Windows exe under Proton draws
through Vulkan. Not drawn yet: the translucent list's modifier volumes
(shadows falling on translucent polygons). Frames that have them are
counted in `vf4ft.log`.

**Speed.** The machine's thread no longer waits for the rasterizer:

| Run | Before (software picture) | GPU renderer |
|---|---|---|
| machine thread per frame, silent, unpaced | 7.0 ms | 3.1 ms |
| 12,100 frames, unpaced | 38 s | 16 s |
| 4 pinned cores / 8 threads (`taskset -c 0-3,16-19`), with the sound chip | 9 to 10 ms, up to 18 | 4.4 to 5.7 ms, up to about 12 |

The Steam Deck's estimate was the pinned run's ×2.2 (section 15's
measurements). That put the software picture at about 17 ms a frame, over
the 16.7 ms a frame has. With the GPU renderer it comes to about 10 to 12
ms. This is an estimate: no Deck has run it yet.

**The Steam Deck route.** The Windows release (launcher, SDL3.dll, Python's
embeddable package) is added to Steam as a non-Steam game with Proton and
runs in Game Mode. The D-pad and A work the launcher's buttons. With no
set chosen it looks for one in the usual places: EmuDeck's ROM folders on
the Deck's own storage or an SD card, and a home's Games, roms, Downloads
and Desktop folders. Under Proton it reaches the Linux side through Wine's
Z: drive. Play starts the game in the same Proton prefix, and the launcher
hides until the game ends. The game goes fullscreen by itself (`fullscreen
= auto`), Steam Input's pad is player 1, and holding View + Menu for a
second opens the game's menu (section 21; it quit the game until then).
The native Linux launcher still links the system's SDL3, so
the Windows release is the one for the Deck.

**Test mode.** On the board, Test makes the game call BIOS service 0x44;
the BIOS's system menu takes over, and its game test mode is `test.bin`,
the separate test program (2,120 routines), whose EXIT calls 0x44 again.
Our BIOS stand-in has no system menu, so 0x44 goes straight to the other
program: `test.bin` from the game, the game from `test.bin`'s EXIT.

- The switch (`program_switch` in `rt_native.c`): service 0x44 calls
  `hle_program`, which jumps back to `rt_native_run`'s frame, so nothing of
  the old program is on the stack. The machine goes back to its power-on
  state, kept when the game starts (`rt_native_power_on`), less what the
  cabinet keeps with the power off: the backup RAM, the I/O board's EEPROM
  and the cards. The frame and instruction counts go on; the virtual clock
  starts again. For the test program its bytes come from the Naomi
  header's load table (0x3c0: image 0x360000 to 0x0c020000, 0x80000
  bytes), as the BIOS loads it. Then the running program's tables take
  over: dispatch (`disp_program`, `disp_init`), the stand-in's vectors and
  hooks (`hle_init`), the translated blocks (`T_` for the test program,
  `M_` for the game), the observer's mode words and the view's tables.
  The game window saves the backup and the cards at each switch.
- `test.bin`'s tables: `tools/sync_meta.py` writes `meta/test/globals.tsv`
  from the decomp's `src_test` globals (769), and `tools/make_tables.py`
  writes `tables_test.c` beside `tables.c`; both programs are linked into
  the one executable.
- The test program's exception entry (its bytes are main.bin's) is
  named `exception_entry` in the decomp's map since decomp 794d491f; the
  stand-in still takes `irq_entry_via_firmware` - 0x44, where it is in
  both programs, for a map without the name.
- **Workaround, fixed in the decomp:** the decomp's test map left out the
  eight Kamui hook stubs at 0x0c06b040 + 0x20 n (byte for byte main.bin's
  at 0x0c141000 to 0x0c1410e0), so the stand-in had no hook on the test
  program's busy wait, which spun through the interpreter: 7.5 ms of the
  machine's time a frame. `make_tables.py` used to add them
  (`TEST_EXTRA`). Decomp 794d491f names them (and the exception entry) in
  `ghidra/source_map_test.tsv`, `meta/test/routines.tsv` has them from
  there, and `TEST_EXTRA` is gone. The busy wait is hooked as before:
  headless (`--sound chip --cards`), 3,000 test mode frames add 1.0 s to
  the 4.96 s of the boot alone, 0.33 ms a frame (the workaround's run
  measured 0.82 ms the same way). `work/gen/test` translated again from
  the new list: 2,128 routines (2,119 before; the nine new ones and
  nothing else). Over the same 3,000 test mode frames the guest runs the
  same 2,182,233,602 instructions to the same place; the calls that leave
  translated code fall from 123,916 to 49,981 and the instructions
  interpreted from 5.90 to 5.60 million. The test mode tour (its screens,
  EXIT, attract 6,312 vertical blanks later, test mode again) stops at the
  same frame and mode at every step as before.
- The test program has no sound (the sound test is the BIOS's system
  menu): the sound CPU stays in reset while it runs, as on a cold start of
  that program. This found a bug in M4's code: the sound chip's state was
  registered for the snapshots with size 0 (`n` was read before
  `aica_state(&n)` set it, in the same call's arguments), so the power-on
  state put sound RAM back but left the game's ARM and effect DSP running
  over it: undefined ARM instructions, and full-scale noise from the DSP
  after EXIT. Fixed in `hw_sound.c`; the controller's sandbox and the
  script's snapshots had the same gap.
- The BIOS services the plan expected the test program to need (0x04,
  0x48, 0x4c, 0x50, 0x54, 0x5c) were not called on any screen; one that
  is called is answered 0, with a `[bios] service ... has no model`
  message.

Checked headless (the software picture) and in the window (the GPU
renderer): every item of the menu runs (input test, game assignments,
arrival stage, character and total data, VS diagram, card menu, network
test, close setting, book keeping, backup data clear) with no hardware
access outside the models; the backup RAM, where the game assignments
are kept, is carried over both switches; EXIT starts the game, which
boots in the same 6,312 vertical blanks as from power on and reaches
attract in the same state as a cold boot (the sound chip's included).

Controls: Test is F2 and Service F3 by default; both are rebindable in the
launcher's Controls page ("Test (operator)" and "Service (operator)") and
have no gamepad default. In the menu Service moves and Test chooses;
screens such as the input test end with Service and Test together. The
coin and free play settings are not in the game's test mode (they are the
BIOS's); they stay in the settings file.

## 21. The game's menu (2026-11-12)

A menu over the picture, as Hookshot's recomps have, so a player with only
a gamepad can change settings, reach the operator's test mode, restart or
quit without the launcher or a keyboard. `test.bin` is not rewritten into
it: the test mode stays the game's own program (section 20), and the menu
only presses its switches.

**Opening it.** Insert or Esc on the keyboard; a gamepad's Guide button
(Xbox, PS); View + Menu held for a second on either of the first two pads,
which is the way on the Steam Deck, whose Steam button Steam keeps. The same
key or button closes it, and so do B and Esc (back). While it is open the
machine waits in the frame hook before its pacing (`menu_pause`), so the
paused time is not counted as the machine's, and the audio device pauses.
The game itself is not told: it is stopped between two vertical blanks.

**Using it.** Up and Down choose (D-pad, left stick or the arrows; a held
direction repeats after 350 ms, every 110 ms), Left and Right change, A or
Enter selects. Any pad steers, so a second player can too. The rows:

| Row | What it does | When it applies |
|---|---|---|
| Resume | closes it | |
| Credits | free play or arcade coins | at a restart or the next start (the game reads the EEPROM as it starts) |
| Picture, Scaling, Fullscreen | 4:3 or stretch, smooth or sharp, auto/off/on | at once |
| Volume | the game's sound, 0 to 100% in tens (new `volume` setting) | at once; shown as "(sound off)" with the sound chip off |
| Renderer, Resolution, Card units | GPU or software, 1 to 4 times 640 x 480, the card units | at the next start; marked `*` while they differ from this run |
| Operator test menu | presses the Test switch, then the operator bar | at once |
| Restart the game | power off and on: the game starts again | at once, after a second A |
| Quit | saves and closes | at once, after a second A |

The settings file is written when the menu closes, if anything changed.
The line under the rows says what the chosen one does.

**Test mode from a gamepad.** Operator test menu holds the Test switch for
six vertical blanks, which starts the test program, and shows the operator
bar at the top of the picture: A is Test and X is Service until B closes
it or the test program's EXIT starts the game again. F2 and F3 work as
before. The bar is drawn for 6 seconds; its buttons work until it closes.
Opened in the test program, that row is Operator buttons and only brings
the bar back. The cost: while the bar is on, the pad's A, X and B are the
operator's, so the input test cannot show them; B closes the bar for that.

**Restart.** `game_machine_restart` puts the coin setting in the EEPROM,
then `rt_native_restart` takes the same `longjmp` as the test program's
EXIT (section 20) to the power-on state, keeping the backup, the EEPROM and
the cards; the backup and the cards are saved first. It runs from the
frame hook, which is safe because nothing is half done there: the picture
of the frame is drawn before the hook (the view's pool waits) and the
event list keeps no state across a call.

**Handing the controls back.** A key or button held as the menu closes
(the A that chose Resume, the View + Menu that opened it) stays the menu's
until it is let go, so it does not reach the game as a throw or a coin.
The chord itself never reaches the game while both are held.

**Drawing.** `runtime/host/menu.c` draws the panel (320 x 216) with SDL's
software renderer and its 8 x 8 debug font into a surface. The surface is
uploaded to a texture, and blitted over the swapchain after the picture
with nearest filtering at the largest whole scale that fits 85% of the
picture (3 times at 1280 x 960). No shaders, so it works on whatever
device the window has, the software picture's included.

**Tests.** `--keys "FRAME:item,..."` presses keys one after another once
the machine reaches vertical blank FRAME: SDL key names, `@N` (wait for
vertical blank N), `shot=FILE` (the window as shown, the menu included),
and `pad:a+x=N`, buttons of a virtual gamepad (SDL's virtual joystick)
held for N turns of the window's loop. The virtual pad is attached at
start-up: SDL sends no press for a button already down when a pad is
opened, so a pad made at its first press missed it.

Checked in a window on Linux with the GPU renderer: every row; the live
settings applied and saved; Test into the test program with the bar, the
bar's X and A through the input test and out with X + A, B closing it;
Restart from the test program, with arcade coins, back to INSERT COIN(S);
Quit; View + Menu, Guide and the D-pad's repeat on the virtual gamepad.
The Windows exe under Proton draws it through Vulkan the same way. Not
done: a real gamepad and the Steam Deck (the virtual pad goes through the
same SDL gamepad events, but Steam Input was not in the way), and changing
controls from the menu (the launcher's Controls page does that).
