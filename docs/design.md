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
2. `vf4ft-launcher` (`runtime/launcher/launcher.c`, SDL3 and `runtime/ui`
   only): Game files › Build › Play, with Update, Settings and Add to Steam;
   it runs step 1 (with `--launcher`: progress lines, cancel by closing
   its standard input, section 22) and `tools/steam_shortcut.py`. The Windows release
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
`SteamDeck=1` or `SteamTenfoot=1`), `window` (auto or a height in pixels,
section 40), `picture` (4:3 | 16:10 | 16:9 | 21:9 |
stretch; the wide ones since section 39), `filter`
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
launcher's Settings, Keyboard tabs ("Test (operator)" and "Service (operator)"), and
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
Enter selects; Q and E (Page Up and Page Down, LB and RB) switch its tabs,
Game, Picture, Sound and Controls (section 22). Any pad steers, so a second
player can too. The rows:

| Row | What it does | When it applies |
|---|---|---|
| Resume | closes it | |
| Credits | free play or arcade coins | at a restart or the next start (the game reads the EEPROM as it starts) |
| Picture, Scaling, Fullscreen, Window size | 4:3, 16:10, 16:9, 21:9 (section 39) or stretch, smooth or sharp, auto/off/on, the window's size (section 40) | at once |
| Volume | the game's sound, 0 to 100% in tens (new `volume` setting) | at once; shown as "(sound off)" with the sound chip off |
| Renderer, Internal resolution, Card units | GPU or software, 1 to 4 times 640 x 480, the card units | at the next start; marked with a dot while they differ from this run |
| Operator test menu | presses the Test switch, then the operator bar | at once |
| Restart the game | power off and on: the game starts again | at once, after a second A |
| Quit | saves and closes, and the launcher that started it (section 40) | at once, after a second A |

The settings file is written when the menu closes, if anything changed.
The panel beside the rows says what the chosen one does. The Controls tab
lists the controls (the keyboard's or the gamepads', as the last input
was); they are changed in the launcher.

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

**Drawing.** First a 320 x 216 panel in SDL's 8 x 8 debug font, scaled
up by whole steps; since section 22, `runtime/ui` at the window's own
pixel size, over a blurred copy of the picture. Either way SDL's software
renderer draws it into a surface that is uploaded to a texture and blitted
over the swapchain after the picture. No shaders, so it works on whatever
device the window has, the software picture's included.

**Tests.** `--keys "FRAME:item,..."` presses keys one after another once
the machine reaches vertical blank FRAME: SDL key names, `@N` (wait for
vertical blank N), `shot=FILE` (the window as shown, the menu included),
and `pad:a+x=N`, buttons of a virtual gamepad (SDL's virtual joystick)
held for N turns of the window's loop. `padtype=NAME` anywhere in the
list gives that pad a real pad's ids, so its glyphs show (section 23). The virtual pad is attached at
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
controls from the menu (the launcher's Settings does that).

## 22. The launcher's and the menu's look (2026-11-12)

Both were redrawn in the manner of Hookshot's recomp builder and in-game
overlay, on request: a stepper, a progress card and a log for the builder;
tabs switched with Q and E or the shoulder buttons, rows with ◀ value ▶
and Off/On, a panel that explains the chosen row, and key caps at the
foot. Only the layout ideas are taken. The name, colors, fonts and code are
this project's own (Hookshot's are not copied), and nothing in it comes
from an emulator.

**runtime/ui** (`ui.h`) is one small kit for both, drawn with SDL's 2D
renderer: the launcher's window renderer, and in the game the software
renderer that draws the overlay (section 21).

- *Units.* Pages are laid out in logical units, and a pixel is offset +
  unit x scale; `ui_fit` letterboxes a page of W x H units into the target.
  Text, rounded rectangles and icons are made at the pixel size they show
  at, so nothing is scaled after it is drawn and nothing blurs.
- *Fonts.* Noto Sans (Regular, Medium, Bold) and Noto Sans Mono, cut down
  by `tools/make_fonts.py` to Latin-1 and a little punctuation and kept in
  `fonts.c` as C arrays. That is 240 KB of source and no `#embed`, so the
  player's own compiler builds it for the native Linux target. stb_truetype
  1.26 packs each face at each pixel size into a sheet the first time it
  is used. A text's y is the middle of its capitals, so text centers in a
  box without fiddling. Their license, SIL OFL 1.1, is `runtime/ui/OFL.txt`.
  The release carries it in `builder/`, and the builder copies it into the
  game's folder as `Font license.txt`.
- *Shapes.* Rounded rectangles come from quarter-disc textures plus fills.
  Icons are made from signed distance functions at their size: arrows,
  check, chevron, play, and the pads' shapes (section 23).
- *Input.* `ui_note_event` keeps the device last used: the keyboard, or a
  pad in its own style (section 23). The key caps and footer hints use
  that device's names, such as Enter and Esc, A and B, or ✕ and ○, and
  they change as soon as another device is used.
- *Widgets.* Tabs with their previous and next caps; rows (action, choice,
  Off/On, slider, a control's binding, with a dot for "applies at the next
  start"); the about panel; a progress bar (indeterminate when the stage
  gives no number); footer hints.
- `options.c` holds the player's settings as rows, with one text and one
  step for each, shared by the launcher's Settings and the game's menu.

**The launcher** is 960 x 600 units, letterboxed into any window. Its pages:

- *Game files.* The set chosen or found, with Choose folder… and Continue.
- *Build.* Back and Build.
- *Working.* The stage's name, "Stage N of 6", a bar, the elapsed time,
  the builder's last lines, and Cancel.
- *Stopped.* The error lines, "Build cancelled" when it was, and Try
  again.
- *Play.* Play, Settings, Add to Steam, Build again and Game files, plus an
  Update card when the builder (`tools/vf4build/__init__.py`) is newer than
  the game it built (`build.json`).
- *Settings.* Tabs Game, Picture, Sound, Gamepad, Keyboard 1 and
  Keyboard 2. A control's row takes the next key or button; Del or X clears
  it, and Backspace or Y restores the default.

The stepper at the top lights the page's step and checks the steps that are
done. The mouse, the keyboard and a gamepad all work every page.
`--shot PAGE FILE` draws a page offscreen for tests, and `--cancel-after S`
presses Cancel. For the README's pictures, `--shot-device NAME` picks the
style of the caps (`keys` or a style's name, section 23), and `--shot-scale K` makes the window K times
as big from the start. The offscreen driver keeps its first framebuffer
when a window is resized later, so a later resize drew only a corner. On
Linux a path under the home folder shows as `~/…`.

**The builder's side** (`make_game.py --launcher`).

- *Progress.* Six stages: reading the set, the compiler, translating,
  compiling, linking, installing. Each is announced on standard output as
  `@stage N 6 TEXT`, and its progress as `@pct P` from the download,
  extraction and compile loops. The compile counts files as they finish
  (`as_completed`), not in submission order, which held the bar until the
  end. The launcher shows the other lines in its log box, and the `error:`
  lines on the stopped page.
- *Cancel.* The launcher closes the builder's standard input. A thread in
  the builder (`tools/vf4build/proc.py`) reads it, and at its end kills
  every child and exits with 3. The children (the translator and the
  compiler) are started through `proc.run`. On POSIX each one is in its own
  process group, and on Windows it gets `taskkill /T /F`, because
  llvm-mingw's clang is a wrapper that starts clang.exe.
- *Never mid-install.* A cancel waits while the install folder is being
  changed, and once that is done there is nothing left to cancel.
  `build.json` and `dimm.key` are written to `.part` files and renamed, so
  a kill leaves no half file.
- *The launcher's side.* If the builder has not ended 15 s after Cancel,
  the launcher kills it. Closing the launcher during a build cancels it and
  waits up to 10 s. The same end of input comes if the launcher dies, so a
  build never runs on alone.

**The game's menu** is drawn at the window's size in pixels. The overlay's
surface and texture are made again when that size changes, and the 1100 x
620 unit panel is letterboxed into it. The window's smallest size is now
640 x 400, so the menu always fits.

Behind it is the picture as the window last showed it:

1. On the menu's first frame the window shows the picture alone.
2. That frame is composed once more into a texture and read back.
3. The copy is made small (by h / 90) and blurred by a 3 x 3 box twice:
   about a 20-pixel blur at 1680 pixels high.
4. It is scaled back up bilinearly and darkened to 42% over the
   background color.

The copy is taken again when the window's size changes (F11, for one).

The operator bar is a strip across the top of the picture, 6% of its
height and at least 30 pixels.

**Fixed on the way.** After a good build the launcher aborted (exit 134):
it closed the builder's input stream again after `SDL_DestroyProcess` had
closed it. It now forgets the stream instead. The first backdrop was
darkened twice, by the blur's mix and again by a fill under the panel, to
about a fifth of the picture's brightness. The fill is gone.

**Checked:**

- The launcher's pages offscreen.
- A full build from the launcher, and a cancel during translation and
  during compiling: the builder exits with 3, and no translator or clang is
  left running.
- The menu in a window over the attract demo, with the GPU and the Software
  renderers: every tab, the dot, and the pad's caps on the virtual pad.
- F11 to 3840 x 2160 and back with the menu open.
- The operator bar.
- The PlayStation glyphs. In the launcher they come from `--shot-device
  ps`. In the game's menu they come from a virtual pad with a DualSense's
  vendor and product ids (`--keys "…,padtype=ps5,…"`), which SDL reports
  as a PS5 pad.
- The Windows build compiles without warnings.
- The Windows release (`package_release.py`) under Proton:
  - `--do build --exit` built the game in 83 s, including the first
    download of llvm-mingw, with `Font license.txt` in the game's folder.
  - `--cancel-after 25` stopped the builder during translation within the
    same second. The built game was kept unchanged, and no Python,
    translator or clang was left running. With `--exit` the launcher ends
    with 1, and its log says "cancelled".

**Not done:** a real gamepad and the Steam Deck (the glyphs follow SDL's
pad type, and only virtual pads have shown them: section 23), and a window resized by dragging while
the menu is open (each new size takes the picture again).

## 23. Glyphs for each kind of gamepad (2026-11-12)

The caps and hints of section 22 name a pad's buttons the way that pad
does. There are nine styles (`ui.c`, `styles[]`), and
`docs/images/glyphs.png` shows every input in each:

| Style | Face buttons | Shoulders, triggers | Back, Start | Guide | Other |
|---|---|---|---|---|---|
| `xbox` (One, Series) | A B X Y, colored | LB RB, LT RT | View and Menu icons | Guide | Share |
| `xbox360` | A B X Y, colored | LB RB, LT RT | Back, Start | Guide | |
| `ps5` (DualSense) | ✕ ○ □ △, white as on the pad | L1 R1, L2 R2 | Create, Options | PS | Mute, Touchpad; the Edge's back buttons |
| `ps4` (DualShock 4) | ✕ ○ □ △, colored | L1 R1, L2 R2 | Share, Options | PS | Touchpad |
| `ps3` (DualShock 3) | ✕ ○ □ △, colored | L1 R1, L2 R2 | Select, Start | PS | |
| `switch` (Pro, Joy-Con) | B A Y X by position | L R, ZL ZR | − and + icons | Home icon | Capture icon; SL SR |
| `gamecube` | A X B Y by position, A green and B red | ZL Z, L R | Back, Start | Home icon | Capture icon |
| `steam` (Deck, Steam Controller) | A B X Y | L1 R1, L2 R2 | View and Menu icons | Steam | the quick access (···) icon; L4 R4 L5 R5 |
| `generic` | A B X Y | LB RB, LT RT | Back, Start | Guide | |

- *Face buttons* are named by position, as SDL maps them: the bottom
  button is A on an Xbox pad, ✕ on a PlayStation one, B on a Switch pad.
  The label comes from the pad itself (`SDL_GetGamepadButtonLabel`), so a
  pad mapped unlike its type (an 8BitDo pad in its Switch layout, say)
  still shows its own letters, and otherwise from the style's type
  (`SDL_GetGamepadButtonLabelForType`).
- *Icons* are drawn from distance functions like section 22's: View (two
  windows), Menu (three lines), +, −, Home (a house), Capture (a square
  with a dot) and the quick access dots. No logo is drawn; Guide is a word
  (Guide, PS, Steam) or the Home icon.
- *Which style.* The pad last pressed, by SDL's gamepad type (`style_of`).
  Under Steam Input, SDL asks Steam which pad is behind Steam's virtual
  one (`SteamVirtualGamepadInfo`) and reports that pad's type. When it
  cannot, the pad is Valve's 28de:11ff, an Xbox 360 lookalike (what M0
  saw under Proton). Such a pad is shown as an Xbox One pad, or as the
  Deck's own controls when the environment has `SteamDeck=1`. That is a
  guess, labeled as one in the code: a DualSense on a Deck behind Steam
  Input would show the Deck's names if Steam does not say what it is.
- *Two players, two kinds of pad.* The caps follow whoever pressed last,
  and each pad's buttons work by its own labels whatever the caps show.
  `ui_note_event` returns 1 when the caps' names change: another style, or
  another pad of the same style labeled otherwise (an Xbox pad and an
  8BitDo pad in its Switch layout). The menu redraws on that; it first
  compared only the style. Checked headless with two Xbox One pads, one
  mapped `face:bayx`, and a DualShock 4.
- `--shot-device NAME` (launcher) takes `keys` or a style's name (`ps` is
  `ps5`), and the game's `--keys` takes `padtype=NAME` for its virtual pad:

  | `padtype=` | Ids | SDL's type |
  |---|---|---|
  | `xbox` | 045e:0b12 (Series X\|S) | xboxone |
  | `xbox360` | 045e:028e | xbox360 |
  | `ps5` | 054c:0ce6 (DualSense) | ps5 |
  | `ps4` | 054c:09cc (DualShock 4) | ps4 |
  | `ps3` | 054c:0268 (DualShock 3) | ps3 |
  | `switch` | 057e:2009 (Pro) | switchpro |
  | `gamecube` | 057e:2073 (Switch 2 GameCube) | gamecube |
  | `deck` | 28de:1205 (Steam Deck) | steam |
  | `steamvirtual` | 28de:11ff (Steam Input's pad) | standard |

**Fixed on the way.** The game's log tagged any Valve pad "(Steam
virtual)", the Deck's own controls included. Only 28de:11ff is tagged now.

**Checked:** each `padtype` in the game on Linux, the Controls tab and its
footer looked at 1:1. Each style showed its own names, `steamvirtual` as
Xbox, and `steamvirtual` with `SteamDeck=1` as the Deck. Linux and Windows
build without warnings.

**Menus go by label, the game by place** (asked for by David). The game's
controls stay where the buttons sit, so the PlayStation 2 layout is the
same on every pad. In the game's menu, the operator bar and the launcher,
a face button does what its label says (`ui_pad_face`, from the pad's own
labels as SDL gives them):

| Label | In menus | Xbox, PlayStation, Steam | Switch | GameCube |
|---|---|---|---|---|
| A or ✕ | select; the bar's Test | bottom | right | bottom |
| B or ○ | back; the bar's Done | right | bottom | left |
| X or □ | the launcher's Clear; the bar's Service | left | top | right |
| Y or △ | the launcher's Default | top | left | top |

A pad whose labels SDL does not know goes by place, as an Xbox pad. The
caps and hints show the button that does the job (`ui_pad_face_name`), so
"Select" shows A on every pad. While the operator bar is up it keeps each
pad's A, X and B from the game by label, so Y stays the game's. Checked
with a headless test of every pad type (each job once, and the caps
matching the pad) and in the game on virtual Xbox, Switch and GameCube
pads: back at Restart closed the menu, select armed it, the bar's caps
were A, X and B, and its B gave the buttons back. The launcher's hints were
checked offscreen; its input was not driven.

**Not done:** real pads other than a DualSense, and Steam Input on a Deck
or under Proton with a real pad (whether Steam tells SDL which pad is
behind its own).

## 24. Updates (2026-11-13)

Asked for by David: check for a new release and update the program before
playing, or have the update go in when the launcher closes.

**The pieces.**

- `release.json` in a release folder: its version, its platform (`windows`
  or `linux`) and the GitHub repository. `tools/package_release.py` writes
  it. A folder without it (a development build) never updates itself, and
  its Play page shows no update row.
- `tools/update.py`, run by the launcher with the release's own Python:
  - `check FOLDER` lists the repository's releases (GitHub's API,
    `/repos/OWNER/NAME/releases`, without signing in; pre-releases count,
    drafts do not). It answers `@update none` or `@update VERSION SIZE`
    for the newest release with a package for this platform,
    `vf4ft-VERSION-PLATFORM.zip`.
  - `fetch FOLDER VERSION --launcher` downloads that package into
    `update/download.part` and checks it against the SHA-256 GitHub lists
    for it (the asset's `digest`) and its size. It unpacks it into
    `update/new.part`, refusing any path outside it, and checks that it
    is a release folder of this platform at that version, with a launcher,
    `make_game.py` and `update.py`. Then it renames it to `update/new`,
    writes `update/ready.json` and prints `@ready VERSION`. It prints
    progress as the builder does (`@stage`, `@pct`), and its input closing
    cancels it, as with the builder (`proc.watch_stdin`).
  - It changes nothing outside `update/`.
- The launcher puts `update/new` in place (`apply_update`). Each entry of
  `update/new` (the launcher, SDL3.dll, `python/`, `builder/`,
  `release.json`, `READ ME.txt`) takes the place of the folder's own, which
  is renamed into `update/old-TIME/`. Renaming rather than writing over is
  because Windows lets a running exe and its loaded DLLs be renamed, not
  overwritten. A step that fails moves back what moved, and the card says
  so. `game/`, `launcher.log` and the set's path are not touched, so the
  saves and settings in `game/user/` stay. It then writes
  `update/applied.json` (from, to).

**When.**

- At every start the launcher runs a check: one request to GitHub, no
  download. A newer release puts a card on the Play page, in the place of
  the "new builder" card: *Version X is out*, with **Update now** and
  **When I close**.
  - *Update now* downloads it, puts it in place as soon as no build is
    running, and starts the new launcher, which closes the old one.
  - *When I close* downloads it now; the card then says it goes in when
    the launcher closes, with *Update now* still there.
  - Either way, the next launcher finds `applied.json`, adds to the old
    log instead of starting a new one, and builds the game again with the
    new builder (the "new builder" Update, with the set no longer needed).
  - A failed download or swap shows the reason and *Try again*.
  - Closing the launcher during a download cancels it; the next start's
    check finds the release again.
- Without a card, the foot of the Play page has **Check for updates** and
  the last check's result (*Up to date*, *Could not check for updates*). A
  check asked for by the button puts the reason for a failure in the
  status line.
- The apply waits for `update.py` to end even when the launcher is
  closing, because its Python must not be running from `python/` when
  `python/` is renamed.

**Clean-up.** Each check removes `update/old-*` (the old launcher has ended
by then), unfinished `*.part` downloads, and a downloaded update that is
no longer newer. One exception: while `update/new` is still there (no
update went in since), an `old-*` folder with files in it is what a failed
swap could not move back. It is then the only copy of those files, and it
is kept. Links are removed, never followed.

**For tests.**

- `VF4FT_UPDATE_API` replaces `https://api.github.com`.
- `--do check`, `--do upgrade` (Update now) and `--do upgrade-at-close`
  (When I close; the launcher then closes as soon as the download is in)
  drive it. With `--exit`, the result is 0 when it updated (or, for
  `check`, when GitHub answered) and 1 otherwise.
- `--shot update`, `update-fetch`, `update-ready`, `update-failed` and
  `update-none` draw the card's states with a made-up 0.6.0.
- `package_release.py ... --version V` packages the tree as version V.

**Checked** against a stand-in for GitHub: a local server with release
lists (a draft, a release for the other platform only, an older one, a
wrong digest, and a throttled download).

- `update.py`:
  - it skipped the draft and the other platform's release;
  - it caught the wrong digest;
  - a check removed the leftovers and kept a newer download;
  - closing its input mid-download cancelled it (exit code 3);
  - a private repository's 404 gives a message that says so.
- The launcher, offscreen, natively with Linux packages and under Proton
  with Windows ones:
  - *Update now*: 0.5.1-dev to 0.5.2; the old launcher swapped the files
    and started the new one, which built the game again (66 s natively,
    70 s under Proton) and cleaned `update/`.
  - *When I close*: 0.5.2 to 0.5.3; the swap at close, then the next
    start's rebuild.
  - A forced failure (`update/new` read-only) moved `builder/` back,
    leaving the folder as it was.
  - umu-run kept running until the restarted launcher ended.
- The card's states were looked at 1:1. Its text first ran past its edge,
  so the card now grows with its lines (`ui_wrap_lines`).

**Not done.**

- The repository is private, and GitHub shows a private repository's
  releases only to someone signed in, so every check fails until the
  releases are public (this repository, or one that holds only releases).
- Real Windows: Wine allows the renames, and Windows documents that a
  running exe can be renamed, but this was not tried on Windows itself.
- Steam's own tracking across the restart (umu-run's was).
- The game added to Steam with Add to Steam starts without the launcher,
  so it never checks by itself. Its menu has the check instead (section
  25).
- Release 0.5.0 has no updater: going from it to the first release with
  one is by hand.
- The SHA-256 comes from the same GitHub answer as the download, so it
  catches a damaged download, not a changed release. There is no
  signature.

## 25. Updates from the game's menu (2026-11-13)

Asked for by David: Add to Steam starts the game without the launcher, so
the game's own menu should have a check for updates, with the update
going in when the game closes.

**In the game** (`runtime/host/updates.c`, the menu's Game tab, before
Quit):

- The row is there only when the game is a release's: its folder's parent
  has `release.json`, `builder/tools/update.py`, the launcher and, on
  Windows, `python/pythonw.exe`. A development build has no row.
- **Check for updates** runs the release's `update.py check`, as the
  launcher does, while the menu stays open. The row then says *Checking…*,
  *Up to date* or *Could not check* (the reason is in the panel beside it).
- A newer release makes the row a switch: **Update to X when I quit**.
  On writes `update/at-game-quit` (`{"version": X}`) in the release
  folder; Off removes it. The game itself downloads nothing.
- When the game quits, after its window is gone and the records are saved:
  - If the launcher started it, it does nothing more. The launcher gives
    the game `VF4FT_LAUNCHER=1` in its environment. That is an environment
    variable, not an option, so an older game ignores it.
  - If it started on its own (from Steam), it starts the launcher with
    `--after-game` in the background, and exits.
- A request left from an earlier run (the game killed before it could hand
  it over, by Steam's Exit game for one) shows as On, and goes at the next
  quit.

**In the launcher:**

- `take_game_request`, when the launcher starts and when the game it
  started ends, removes `update/at-game-quit`. It then does Update now
  after a fresh check (`upd_auto`): download, swap, restart, rebuild.
- With `--after-game` the launcher closes once that is done and the Play
  page shows. A failure (check, download, swap or build) stays on screen
  with its reason. It passes `--after-game` to the new launcher, which
  skips its own check, builds the game again and closes.
- Started from the launcher's Play, the launcher comes back when the game
  ends, does the update, and the new one stays open on Play.

**Fixed on the way.** The first run of the game's path closed the
launcher without updating. A check's `update.py` prints `@update` a
moment before it exits, and the step pressed Update now in that moment:
the download did not start while the check's process was still there, and
the one-shot step was spent. Update now and When I close are now enabled
only once no `update.py` runs, and the step waits for that too.

**Checked** against a stand-in for GitHub:

- Natively: the Linux game started on its own, its menu driven by
  `--keys`. Check found the newer release; Update when I quit wrote the
  request; Quit started the launcher. It downloaded, swapped and restarted,
  and the new one built the game again with the new builder (68 s) and
  closed.
- The launcher's Play, offscreen, with a stand-in game: a script that
  recorded `VF4FT_LAUNCHER=1` and wrote the request. When it ended the
  launcher updated, and the new launcher built the game and stayed open.
- Under Proton: the Windows game started on its own, as from Steam, the
  same way. The update and rebuild (70 s) went through, and umu-run stayed
  up until the launcher closed.
- The menu's row in each state was looked at 1:1, natively and under
  Proton.

**Not done.**

- Steam itself and a real Deck. Whether Steam keeps the game running while
  the launcher works is shown only by umu-run's waiting.
- A game killed before it hands the update over (the request then waits
  for the next quit or launcher start) was not tried.
- The real check needs the releases to be public (section 24).

## 26. Ko-fi in the game (2026-11-13)

Asked for by David: his Ko-fi page in the game's menu, and once when the
game starts, as in his pcbview (a one-time notice at its first start, and
Help → Support on Ko-fi).

**The menu.** The Game tab has **Support on Ko-fi** before Quit. Choosing
it opens `https://ko-fi.com/P5P81EV1M0` in the browser (`SDL_OpenURL`), and
the menu stays open. The panel beside it says what the page is, gives the
address to type in elsewhere (a Deck in Game Mode may have no browser to
show), says *Sent to your browser* or *No browser opened*, and shows Ko-fi's
own button at its foot. (On Linux SDL counts the address as sent once the
portal or `xdg-open` has it; whether a browser then opens it, it cannot
say.)

**The notice.** The first start shows, once, a panel in the menu's place:
*Support this recomp*, a line on what it is, Ko-fi's button, the address,
and where to find it again. It has two buttons, **Support on Ko-fi**
(opens the page and closes it) and **Close**. Close has the focus, so a
button already held as it comes up opens nothing. Esc, Insert, Guide and
Back close it too. Like the menu, it pauses the game.

- It comes two seconds after the first picture: during the start-up check
  (WARNING.INIT), which waits under it.
- `user/kofi-notice` marks it shown and is written when it is shown, so a
  game killed while it is up does not show it again. Removing the file
  brings it back.
- A test run (`--frames`, `--shot`, `--press`, `--keys` or `--unpaced`)
  never shows it, so earlier tests' key lists still hold. `--kofi-notice N`
  shows it at vertical blank N in any run, and writes no mark.

**The button.** `runtime/ui/kofi_badge.png` is pcbview's copy of Ko-fi's
button (672 x 356). `tools/make_kofi.py` turns it into a C array
(`runtime/ui/kofi_badge.c`), and `ui_png` draws it with SDL 3.4's
`SDL_LoadPNG_IO`. The texture is kept until `ui_quit`. An older SDL (a
distribution's 3.2) has no PNG loader: the button is then a plain one in
Ko-fi's orange with the same words.

**Checked.**

- Natively, with `--kofi-notice` and `--keys`: the notice, Left to Support
  on Ko-fi, Return; then the menu's row and Return. Both opened the
  address. Under a desktop's session bus SDL hands the address to the XDG
  portal before it tries `xdg-open`. So for a test that must not open a
  browser, `DBUS_SESSION_BUS_ADDRESS` points nowhere and a stand-in
  `xdg-open` comes first in PATH; it recorded both addresses.
- Two plain starts with a new user folder: the first showed the notice
  after three seconds and wrote the mark, and the second did not show it.
- Under Proton, the Windows build: the notice and the row look the same
  (looked at 1:1). Nothing was opened there, since Wine hands the address
  to the desktop's browser.

**Not done.** A real Windows PC, a Deck in Game Mode (where the browser
opens), and the old-SDL fallback button (both of this project's SDLs are
3.4).

## 27. Install folder, set choice and plain Wine (2026-11-13)

David ran the 0.5.0 release's launcher under plain Wine 11.21 (not Proton)
and reported three things: no way to change where the game goes, the
launcher taking his ROM folder without asking, and a build stuck at
"compiled 50 of 111" for 15 minutes.

**The set is offered, not taken.** `find_set` used to choose the first set
it found, save it, and open the Build page, so the Game files page never
showed. Now it only remembers what it found (`found_path`). The Game files
page says *A vf4tuned set is in this folder*, with the path and two
buttons: **Use this set** (the focus, so the Deck's A still does it in one
press) and **Choose another folder…**. `launcher-set.txt` is written only
when the player chooses.

**The install folder can be changed.** The Build page's Install folder row
has **Change…** (a folder dialog). The choice is kept in
`launcher-game.txt` beside the launcher; with none, the game goes in
`game/` as before.

- The builder writes fixed names in the install folder (`build/`,
  `data/`, `user/`, the program, `READ ME.txt`, `build.json`...) and deletes
  `build/` at the end. So a picked folder is used as it is only when it is
  empty or holds nothing but those (an earlier install). Any other folder
  gets a `VF4 Final Tuned` folder inside it, and the page says so. When
  that folder too has other files, nothing changes and the page says why.
- `tools/make_game.py` refuses such a folder itself (`INSTALL_NAMES`; the
  launcher's `install_names` lists the same, with what the game and Wine or
  Proton add beside it: `vf4ft.log`, the vkd3d and DXVK caches). An
  `--update` of an existing install is not checked.
- Leaving an install that has saves: at the next Build, its `user/` is
  copied into the new folder, unless that folder has its own. The old
  folder is left as it was. The Build page says where they come from.
- The game finds its release folder for **Check for updates** as its
  folder's parent (`game/`). An install elsewhere could not, so the builder
  now writes `"release"` (with `/` separators, since the game's JSON
  reader does not unescape) into `build.json`, and `gu_init` reads it when
  the parent has no `release.json`.

**The stuck build was a frozen window.** The build had finished: 82
seconds, `the game is built` in `launcher.log`, `game/vf4ft.exe` there.
The window still showed "compiled 50 of 111", and the launcher used about
60% of a core. The journal had six `d3d12_swapchain_resize_buffers` lines
at 08:08:32, one second after "compiled 50" (08:08:30.7): the window had
been resized, and nothing after that reached the screen.

- Under plain Wine on this machine, SDL's first renderer, Direct3D 11
  (Wine's wined3d), does not start. SDL then takes Direct3D 12 (Wine's own
  vkd3d, not vkd3d-proton).
- Reproduced with the development launcher. After six resizes through a
  KWin script, the window shows a shrunken old frame in its corner and
  black elsewhere. `SDL_RenderPresent` reports no failure, so the frames
  are lost after SDL hands them to Wine's D3D12 swapchain.
- Forced with `SDL_RENDER_DRIVER`, the Vulkan and OpenGL renderers drew
  correctly after the same resizes. The GPU renderer's run was not seen.
- Under Proton, DXVK gives SDL a working Direct3D 11, which every earlier
  test used.

So the fault is in Wine's Direct3D 12 presentation on this system, not in
the launcher's code. The launcher now logs its renderer and video driver
at start, a present that fails (and when it works again), device
resets, and the first window sizes, so the next report has that in
`launcher.log`. David's decision: Proton only. The launcher keeps SDL's
own renderer choice (no preference for Vulkan over Direct3D 12 to get
round plain Wine), and the README says plain Wine is not supported. Not
looked into, for that reason: why wined3d's Direct3D 11 does not start,
and the game's window under plain Wine (it draws with SDL_GPU, Direct3D
12 first).

**Checked** (offscreen, no window; the dialog's answer given by the test
option `--pick-game DIR`):

- The Game files page with a found set.
- Change into an empty folder, into a folder with other files (a
  `VF4 Final Tuned` folder inside it), and into one whose
  `VF4 Final Tuned` folder has other files (refused).
- A real build through the launcher into a new folder: the old `user/`
  was copied in, and `build.json` named the release folder.
- `make_game.py --out` on a folder with other files stopped before
  writing anything.
- `gu_init` (in a small harness) found the release through `build.json`,
  found none for an install without it, and logged a release folder that
  was missing.

**Add to Steam only from outside Steam** (David's decision, the same
day; replaced by section 33, which shows the button everywhere and asks
for Steam to be closed). `tools/steam_shortcut.py` edits Steam's shortcut files, which Steam
rewrites when it exits, so it needs Steam closed. Run from Steam, the
launcher could never use it; under Proton the launcher's own Steam entry
is already how the game is played (Play starts it in the same Proton).
Under Wine or Proton the tool would also see the prefix's stand-in Steam
(its registry's SteamPath), not the real one. So `steam_setup` hides the
button when Steam's launch variables are set (`SteamGameId`,
`SteamClientLaunch` or `SteamAppId`; umu-run sets `SteamGameId` too), or
when the Windows launcher finds `wine_get_version` in ntdll, and says
which in `launcher.log`. Checked offscreen: the native launcher shows it,
and hides it with `SteamGameId` set; the Windows launcher hides it under
umu-run (through `SteamGameId`) and under plain Wine with no Steam
variables (through Wine). Not tried: Steam itself on Windows, whose launch
variables are expected to be the same ones.

## 28. The launcher's first window size (2026-11-13)

David's report: under CachyOS (KDE, a 4K screen scaled 175%) the launcher
opened small. The Windows launcher under Proton is an X11 window through
Xwayland, and KWin lets X11 programs scale themselves. SDL's window units
are logical on Wayland but pixels on Windows and X11, where
`SDL_GetDisplayContentScale` gives the desktop's scale (SDL's
README-highdpi); the launcher asked for 960 x 600 and applied no scale. So:

- On a high-DPI Windows PC it opened small too (our bug).
- Under Wine and Proton the scale SDL sees is Wine's (`LogPixels` 96 in
  the prefix, scale 1.00), not the desktop's: a probe gave 3840 x 2160 at
  1.00 under both, so the content scale alone cannot fix it there.

`first_size` makes the window the page times the content scale, at least
half the screen's usable height (in the page's 8:5 shape), and no more than
90% of the usable area; the smallest size is 640 x 400 times the content
scale. Pictures (`--shot`) keep the page's own size. `launcher.log` says
the size it chose, the scale and the usable area.

**Checked** in a headless KWin (`kwin_wayland --virtual` with its own
D-Bus session and config folder; the output's scale set through its
`kwinoutputconfig.json`, since `--scale` alone made the output larger at
scale 1). KWin's own measure of the window, on a 3840 x 2160 screen at 175%
(2194 x 1234 desktop units):

| launcher | before | now |
|---|---|---|
| Windows, Proton (umu-run) | 960 x 600 pixels, about 549 x 343 | 1728 x 1080 pixels, 987 x 617 |
| Linux, X11 | 960 x 600 pixels, about 549 x 343 | 1728 x 1080 pixels (scale 1.75), 987 x 617 |
| Linux, Wayland | 960 x 600 | 987 x 617 |

And on a 1920 x 1080 screen at 100% with a Wine prefix set to 144 dpi (as
a Windows PC at 150%): 1440 x 900, the page times 1.5. Not tried: a real
Windows PC, a Steam Deck, and moving the window between screens of
different scales (not changed here).

## 29. Start the game directly (2026-11-13)

David asked for a Steam entry that plays the game without the launcher.
The launcher cannot make one from inside Steam: Steam rewrites its
shortcuts when it exits, so they can only be written with Steam closed, and
closing Steam ends a launcher that runs under its Proton. So the launcher's
own Steam entry plays the game instead, and Steam's Properties renames the
entry (the README's install steps say so).

- **The switch:** *Start the game directly*, on the Play page (where Play
  starts the game itself: the Windows launcher, or a native Linux build),
  kept in `launcher-direct.txt` beside the launcher. Not shown beside an
  update's or a new builder's card.
- **With it on,** the launcher creates its window hidden, presses Play
  itself, and closes when the game ends with code 0 (since section 40 it
  does that with the switch off too). It shows itself
  instead when nothing is built, a new builder or an update is waiting,
  the game asked for an update when it quits, the game ends with an error
  (the Play page says the code), or the game's menu asked for it.
  `launcher.log` says which. No update check runs at a direct start (the
  game's menu has its own).
- **Open the launcher**, a row in the game's menu (Game tab, before Quit,
  chosen twice like Quit) in a release's game: a game the launcher started
  (`VF4FT_LAUNCHER=1`) ends with code 64 (`GU_EXIT_LAUNCHER`), and the
  launcher shows itself; a game started on its own starts the launcher
  with `--show`, which never starts the game directly.
- **The game's exit code:** the launcher started the game in SDL's
  background mode, whose exit code is always 0 (SDL_process.h; on Linux it
  forks twice, on Windows SDL happens to give it anyway). The game is now
  an ordinary child with no input or output (both are GUI programs on
  Windows, so no console opens).

**Checked:**
- Natively, offscreen, with a stand-in game that ends with a chosen code:
  switch off (the launcher stays), on with 0 (it closes 1.2 s later, with
  the game), on with 64 (it stays, "Open the launcher"), on with 3 (it
  stays, the error on the Play page), on with `--show` (no direct start),
  and on with an older builder in `build.json` (not this time: a new
  builder).
- Under Proton in a headless KWin, a 0.5.1 release folder with this
  launcher and game: with the switch on, KWin had the game's window only,
  and closing it ended the launcher too; a stand-in game ending with 64
  brought the launcher's window; the real game's Open the launcher (by
  `--keys`) ended with 64 when `VF4FT_LAUNCHER=1`, and started the launcher
  with `--show` otherwise, whose window came up although the switch was on.
- The Play page's pictures with the switch off and on, and beside the
  cards (hidden there).

Not tried: a real Steam session and the Deck's Game Mode.


## 30. Steam Input's twin pads (2026-11-13)

David played a release under Steam (Proton) with a DualSense: every
button moved player 1 and player 2 at once.

**Why (from his `vf4ft.log` and Steam's environment):** with Steam Input
on for the game, Steam reads the DualSense itself and gives the game its
own virtual pad (Valve's 28de:11ff, an XInput pad: `XInput#0`, type
xboxone, Steam handle 0). Steam names the real pad behind each of its
slots in a file whose path is in `SteamVirtualGamepadInfo`
(`~/.local/share/Steam/config/virtualgamepadinfo.txt`: `[slot 0]`,
`name=DualSense Wireless Controller`, `VID=0x054c`, `PID=0x0ce6`,
`type=ps5`). Natively, SDL reads that file and the real pad is not
shown. Under Proton, SDL takes it that Wine leaves out the real pads
(`SDL_ShouldIgnoreGamepad` in SDL_gamepad.c: with
`SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD` set and Wine found it
ignores nothing, not even Steam's `SDL_GAMECONTROLLER_IGNORE_DEVICES`),
but Proton shows the DualSense through its HID driver as well. So SDL had
two pads, the virtual one first: player 1, and the DualSense: player 2.

**What (runtime/ui/ui.c, for the game and the launcher):**
- `ui_gamepads()` is `SDL_GetGamepads()` without the pads left out; the
  game's players (`read_controls`) and its menu's D-pad use it.
  `ui_pad_event_hidden()` is called with every event, first: the game's and
  the launcher's loops skip the button, axis, touchpad and sensor events
  of a pad left out.
- A real pad is left out when Steam's file gives its vendor and product
  for a Steam virtual pad's slot, one real pad per virtual pad. The slot
  is SDL's player index (Steam's slot when XInput tells it, else
  XInput's user index), else the virtual pad's place among them. A
  virtual pad is 28de:11ff, or one with a Steam handle (run natively, SDL
  gives it the real pad's ids).
- If the file cannot be read while a virtual pad is there and Steam says
  it filters (`SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1`), every
  real pad is left out: under Steam Input every pad the player uses is a
  virtual one.
- The pads are looked at again when SDL's list changes and every second;
  the file is read again when its time changes (Steam writes it as pads
  come and go). The path is used as Steam gives it; on Windows a Linux
  path that cannot be opened is tried on Wine's Z: drive (Proton opened
  it as it is).
- **Glyphs:** a Steam virtual pad is drawn as the pad the file names
  (`type=ps5`: PlayStation shapes), its face labels by place from that
  type. Before, it was an Xbox pad (or the Deck's, under `SteamDeck=1`).
- The log says which pad is left out and why ("gamepad 5 "DualSense
  Wireless Controller" 054c:0ce6 left out: Steam Input gives it as its
  pad 3 (slot 0)"), and the game's "gamepad added" line has the player
  index.

**Checked:** a test program with SDL's virtual joysticks standing in for
Steam's pad, a DualSense and an Xbox pad (an empty `/dev`, so none of the
machine's own), natively and as a Windows build under Proton (Steam's
file at its Linux path):
- the file names the DualSense: players are Steam's pad and the Xbox
  pad; one press on all three counts twice, not three times; PlayStation
  glyphs; the DualSense taken out and put back is left out again;
- two virtual pads and two DualSenses: both DualSenses left out;
- the file cannot be read: every real pad left out, Xbox glyphs;
- no Steam virtual pad (Steam Input off for the game), or not started by
  Steam: nothing left out;
- the file written after the start: the DualSense is left out a second
  later.
The game itself, offscreen with a stand-in Steam pad (`--keys
padtype=steamvirtual`) and the file, logs the list and its slot.

**On David's machine (2026-11-13),** a 0.5.2-dev test copy added to Steam
with Steam closed (`tools/steam_shortcut.py add`, named), Steam Input on,
Proton Experimental. This time SDL had given Steam's pad the real pad's
ids from the file (`XInput#0`, 054c:0ce6, type ps5, Steam handle set,
player 0; with 0.5.1 it had been 28de:11ff, xboxone, no handle), so the
pad was found as Steam's by its Steam handle; the HID DualSense (player 1)
was left out, in the game and in the launcher:

    gamepad added: id 2 "DualSense Wireless Controller" type ps5 vid 054c pid 0ce6 steam-handle 0 player 1 path \\?\HID#VID_054C&PID_0CE6...
    gamepad added: id 3 "DualSense Wireless Controller" type ps5 vid 054c pid 0ce6 steam-handle dd42f4b857317 player 0 path XInput#0
    Steam's pad list: slot 0 "DualSense Wireless Controller" 054c:0ce6 ps5
    gamepad 2 "DualSense Wireless Controller" 054c:0ce6 left out: Steam Input gives it as its pad 3 (slot 0)

## 31. The mouse in the game's menu (2026-11-13)

David asked for mouse clicks in the game's menu (the launcher had them;
the game's menu took keys and pads only).

**What (runtime/host/menu.c `menu_mouse`, main.c's event loop):**
- The row under the mouse is chosen as it moves (its text shows on the
  right); moving onto another row cancels a Restart, Quit or Open the
  launcher that asked once, as Up and Down do.
- A left click: on a tab or the Q and E caps, that tab; on a row, the
  row's A (an option goes one onward; Restart, Quit and Open the launcher
  ask for a second click); on Ko-fi's badge (shown with Support on
  Ko-fi), the Ko-fi page.
- A right click: on a row, Left (an option one back; nothing on an
  action); anywhere else, Back (the menu closes).
- The wheel moves the choice a row per turn, without wrapping.
- The first start's Ko-fi notice: the mouse over a button moves the focus
  to it; a click is that button (the badge is Support on Ko-fi); a right
  click closes it.
- The menu is drawn over the whole window in pixels; SDL gives the mouse
  in window units (logical on Wayland), so they are multiplied by
  `SDL_GetWindowPixelDensity`. `menu_mouse` puts back the menu's own
  `ui_fit` before `ui_to_logical` (the operator bar draws at another
  scale) and does nothing until the menu was drawn after opening (the hit
  tests are the last drawing's).
- Mouse events are taken only while the menu is open; the operator bar
  takes none.
- For tests, `--keys` has `mouse:KIND@XxY` (move, left, right, up, down at
  a window point).

**Checked** natively, offscreen (1280 x 960, density 1), by `--keys`:
hover chose Credits, a click made it Arcade coins (with its dot); a click
on Picture opened that tab and a right click put Picture from 4:3 to
Stretch; two wheel turns went down two rows; the E cap went to Sound; a
right click outside the rows closed the menu; the badge's click reached a
stand-in browser (no session bus); Quit asked again and the second click
quit, with settings.txt saved. The notice: the mouse over Support on
Ko-fi focused it, a click on Close closed it without opening anything,
and a click on Support on Ko-fi opened the page and closed it.

Not tried: a real mouse, and a density other than 1 (a scaled Wayland
desktop running the native build).

## 32. The DIMM board's memory above the image (2026-11-13)

David asked what the red block behind the first letter of the boot's
network check screen was. The same screen read RESPONSE TIME = -1 FRAMES
and MODE = -1.

**Cause (ours, not the game's):** the game keeps its network-check log in
the DIMM board's own memory. `net_log_storage_init` puts it at the media
limit, 0x0F000000; `game_boot` saves a record there before the screen
(`net_log_status_write`, `net_log_save`: `dimm_write` of 0xB5C bytes at
0x0F004000 through the cartridge port), and the screen reads it back every
frame (`net_log_current_draw`, `net_log_read`, `dimm_read`). VF4debug's
cartridge model (`runtime/machine/hw_cart.c`) has memory only as far as
the image, which ends at exactly 0x0F000000, and `hw_dimm_net.c` modelled
the top 16 MiB only while the network side was on (off by default). So the
save was dropped and the read gave 0xFF bytes: -1 for the numbers, and a
status text (+0xA38, the record's last 292 bytes) with no terminating zero.
`net_log_record_draw` prints it with `%s`, so the text ran on into the
stack past the record: a return address (0x0C020696) and a stack pointer
(0x0DFFBFB4) whose bytes hold 0x0D, a carriage return to the line's start,
then 0x1F, the font's glyph 31, a red block, which landed on the R of
RECEIVE STATUS.

Shown by: a temporary probe in the view printed every triangle over that
pixel (the glyph 31 quad, from list address 0x0BBC0E80, beside the R); a
zero poked over the 0x1F byte took the block away, one byte later did not.

**Fix:** the window's memory (`hw_dimm_net.c`, 0x0F000000-0x0FFFFFFF, zero
at power-on, kept in the snapshots as before) is there whether the network
side is on or off. A real board's 256 MiB module has it either way. Off,
the model still answers no registers, commands or interrupts. The game
writes the record just before the screen reads it, so the start value does
not show.

**Checked:**
- With the network side off, the game touches nothing else above the
  image: one write and then one read a frame for the screen's 1,800 frames,
  over a boot, the attract sequence, a coin start and a versus match
  (temporary logging, removed).
- The screen in the game, software and GPU renderers: RESPONSE TIME = 3600
  FRAMES (60 SECONDS), MODE = 3, (GAME) getIPbyDNS TIME OUT, nothing on
  the R.
- Against VF4debug's binary (not changed): network off, the same hashes to
  frame 4000 (the record is read at frame 4030; later hashes differ by the
  record) and the same mode at every thousandth frame to 20,000; network
  on (its `net_game` script, a whole match), the same 43 exchanges and the
  same final hash.

VF4debug has the same gap; it is left to that repository.


## 33. Add to Steam from the launcher, Steam open or closed (2026-11-13)

David asked for it to be easy: open the launcher, have an option to create
or recreate its Steam shortcut, and when Steam is open, a note asking to
close Steam (and, when Steam started the launcher, to open the launcher
again with Steam closed). It replaces section 27's "Add to Steam only from
outside Steam", which hid the button under Wine or Proton and when Steam
started the launcher.

**What it adds:** the Windows launcher adds itself (`vf4ft-launcher.exe`,
from SDL's base path and the name it was started by), named Virtua
Fighter 4 Final Tuned. Its entry plays the game straight away with *Start
the game directly* (section 29) and keeps the launcher's update checks.
An entry of that name is updated in place, so pressing it again recreates
a deleted or moved shortcut; the same path gives the same appid, so Steam
keeps its settings for it. (Before, the Windows launcher added
`game\vf4ft.exe`; the native Linux launcher still adds the Windows build
that its Play starts through Steam.)

**The button:** on the Play page whenever the game is a Windows build:
Windows, plain Wine and Proton alike. Pressing it gives one of three notes
over the page (the page dimmed; only the note's buttons answer; Esc or
Back closes it):

- *Steam closed:* `tools/steam_shortcut.py add` writes the entry, and on
  Linux sets Proton Experimental for it, then "Added to Steam: Start
  Steam: Virtua Fighter 4 Final Tuned is in your library, set to run with
  Proton."
- *Steam running:* the tool changes nothing and ends with code 3. "Close
  Steam first", with Try again (runs it again) and Close.
- *Steam started the launcher* (`SteamGameId`, `SteamClientLaunch` or
  `SteamAppId` set): no tool runs. Closing Steam ends a launcher under
  Steam's Proton, so the note says to exit Steam, then open
  `vf4ft-launcher.exe` again from its folder with Steam closed and choose
  Add to Steam.

**Under Wine on Linux, the real Steam** (`tools/steam_shortcut.py`): the
Windows Python sees Wine's stand-in Steam through the registry, so when
`ntdll.wine_get_host_version` says the host is Linux the tool works on the
Linux host's Steam instead:

- Steam's folder: the Linux home (`HOME` when it is a Linux path, else
  Wine's `WINEHOMEDIR`), then
  `~/.steam/steam`, `~/.local/share/Steam` or the Flatpak's, each turned
  into a Wine path with `kernel32.wine_get_dos_file_name`.
- Steam running: Wine's `Z:` drive shows the host's `/proc`, and a process
  whose `comm` is `steam` counts (as do Steam's launch variables).
- The entry is the one the Linux tool writes: the exe's Linux path
  (`kernel32.wine_get_unix_file_name`), a bare start folder with a
  trailing slash, and the compatibility tool in `config.vdf`.

So a double-click on `vf4ft-launcher.exe` (the Linux desktop opens it with
Wine) is enough to put the launcher in Steam, set to Proton.

**Checked** (never on Steam's own files: copies of `shortcuts.vdf` and
`config.vdf`, through `--steam` or a stand-in `HOME`):
- The tool, Linux and Windows (the release's embedded Python under plain
  Wine 11.21): with Steam running, both end with 3 and write nothing; with
  Steam hidden by a PID namespace (`bwrap --unshare-pid`), both write their
  copy, and the two copies are byte for byte the same. `list` under Wine
  with no `--steam` finds the real Steam folder (read only).
- The Windows launcher under plain Wine in a headless KWin (`--do steam
  --exit`, a stand-in release and `HOME`): Steam running, the close-Steam
  note; `SteamAppId` set, the note to reopen the launcher and no tool run;
  Steam hidden, the copy's entry of that name updated to the launcher's
  Linux path, Proton Experimental set in the copy's `config.vdf`, and the
  added note.
- The Play page and the three notes, offscreen at 1:1 (`--shot
  steam-close`, `steam-started`, `steam-added`).

Not tried: a real Steam reading the entry after it starts (the format is
the Linux tool's, which Steam has read since M0), Windows itself, and the
Deck, which has no Wine outside Steam: there the README's steps (Steam's
own Add a Non-Steam Game) stay the way in.


## 34. Play's note under plain Wine (2026-11-13)

David asked for a warning when Play is pressed on Linux outside Proton and
Steam: the kinds of problems to expect.

**When:** the Windows launcher under Wine (`wine_get_version` in ntdll)
with none of Steam's launch variables (`SteamGameId`, `SteamClientLaunch`,
`SteamAppId`) and no `STEAM_COMPAT_DATA_PATH` (Proton's; umu-run sets
`SteamGameId` too). `launcher.log` says "plain Wine, not Proton: Play
warns first" and names the host (`wine_get_host_version`).

**The note** ("Playing under plain Wine", over the Play page): Proton is
what the game is made and tested for; controllers go through Wine rather
than Steam Input (no Steam layouts or overlay, and a pad Wine does not know
may not work); the picture and sound depend on the Wine version and drivers;
the launcher's own window can stop updating after a resize (section 27).
Its buttons: **Add to Steam** (the focus: section 33's flow), **Play
anyway** (no note again until the launcher is closed), and **Cancel** (Esc
or Back). Start the game directly (section 29) does not show it: that
switch is turned on from the Play page.

**What plain Wine was seen to do** (Wine 11.21, this machine):
- The launcher: Wine's Direct3D 11 does not start, SDL's renderer takes
  Direct3D 12, and after resizes the window stops updating (section 27).
- The game: SDL_GPU takes Vulkan (the RTX 5070 Ti, NVIDIA 620.70.06), and
  it held 60 frames a second through the attract sequence. David's own run
  (12:28) found his DualSense through Wine's HID. In a headless KWin with
  sound off, six quick resizes, then fullscreen on and off by F11: the
  nested screen showed the picture right at every capture (0.4 s to 9 s
  after each change; the 4:3 bars even), and one earlier capture caught a
  switch half done, gone by the next.

**Checked:** the Windows launcher under plain Wine in a headless KWin:
`--do play` gives the note and starts nothing; with
`STEAM_COMPAT_DATA_PATH` or `SteamAppId` set there is no note. The note's
picture at 1:1 (`--shot wine-play`).

Not tried: sound under plain Wine (the runs had it off), other Wine
versions, and other GPUs (where SDL_GPU might take Direct3D 12 as the
launcher does).

## 35. The network setting, off by default (2026-11-13)

David asked whether networking could be turned off so the start-up's
network screen is skipped, then: "lets turn networking off by default".

**What the game does** (the decomp's `source/vf4lib/libbase/src/system.c`,
`game_boot`): `net_init` with `user_back_data.net_connect`, then it waits
`user_back_data.net_timeout` frames on "NOW NETWORK CHECKING" (2 when the
network is off, 3,600 when it is on), writes its network log, and shows the
log screen ("PLEASE WAIT 30 SECONDS OR START BUTTON TO EXIT") for 1,800
frames. All of it is inside WARNING.INIT, which runs unpaced (section 14).
With the network on there is no service to answer (`hw_dimm_net.c`), so the
log says "(GAME) getIPbyDNS TIME OUT", MODE 3. With it off it says
"(GAME) NETWORK OFF", MODE 2, after 2 frames. `net_connect` is the test
menu's Game Assignments, NETWORK (`source_test/testmode/testmode.c`), and
`user_back_defaults_once` makes it 1.

**Where it is kept:** the game's settings record (`user_back_data`, 13,012
bytes) in the battery-backed RAM, as `sdk/libam/userbkup.c` keeps it: two
headers of four little-endian words (the check, the size, the size padded
to words, 0) at 0x1F8 and 0x208, copy A at 0x218 and copy B right after it.
`sram_save_load` wants the size in both headers, then takes copy A when its
check holds, else B, else starts from the defaults. The check is the
EEPROM's (`eeprom_check`: the data and a zero byte). `net_connect` is at
0x162A in the record. David's backup: both checks hold, both copies equal,
`net_connect` 1.

**The setting:** `network = off | on` in `settings.txt` (default off), on
the launcher's Settings page (Game tab) and in the game's menu (Game tab,
marked with a dot until the next start, like Credits). The game's menu now
has ten rows on the Game tab, so its row height went from 43 to 39
(`menu.c`).

**How it goes in** (`game_machine_network` in `game_machine.c`): after the
backup is loaded, before the machine runs, the record's `net_connect` is
set in both copies, with both headers and checks, as `sram_save_store`
leaves them. The menu's Restart does the same with the setting as the menu
left it. A blank backup has no record yet: the game makes it with its
defaults early in its first start-up step (vertical blank 226 here). The
frame function waits for a record with good checks, puts the network off in
it, and restarts the machine once, as an operator would after changing it in
the test menu. If no record shows before the end of WARNING.INIT, the
network stays on for that run (logged).

The test menu's NETWORK row still works: it holds through EXIT (the game's
own restart) until the game is next opened or restarted from the menu,
which put the setting back in. Reading it back into `settings.txt` was not
done: the menu's thread owns the settings.

**What changes:** with the network off, WARNING.INIT ends around vertical
blank 2,113 instead of 5,711 (about 3,600 fewer; at the real pace about
35 s instead of 97 s). Those frames run unpaced, so the warning screen comes
only a fraction of a second sooner: the log screen still flashes by, now
saying NETWORK OFF. Scripted runs (`--press`, `--keys`, `--shot`) with
frame numbers from before now land about 3,600 vertical blanks later in
the game than they did. The headless runner and the VF4debug comparisons
are not affected: they never load a backup, so the game makes its record
with the network on as before.

**Checked** (the native game in a headless KWin, sound off and SDL's dummy
audio; copies of David's backup):
- David's backup, default settings: "network: off", the log screen at
  vertical blank 1,200 says "(GAME) NETWORK OFF", MODE 2; the saved backup
  has `net_connect` 0 in both copies and both checks hold.
- A blank backup: the record appears at vertical blank 226, the network
  goes off in it and the machine restarts once; the same log screen; the
  saved backup has 0 in both copies, checks good; the game reached the
  attract sequence (ADVERTISE) by vertical blank 3,200.
- `network = on`: the log screen at 4,600 says "(GAME) getIPbyDNS TIME
  OUT", MODE 3, as before; the record keeps 1.
- The menu: Network switched on at vertical blank 2,400 (the dot and "Applies
  when the game restarts" shown), then Restart: `settings.txt` says
  `network = on`, the game restarts with the long network wait, and the
  record has 1 in both copies.
- The pictures at 1:1: the game's menu (ten rows) and the launcher's Settings
  page. The launcher's `--shot` inside the nested Wayland KWin read back
  empty (OpenGL renderer); with `SDL_VIDEO_DRIVER=offscreen` it is right.

Not done: hiding the fast-forwarded start-up itself (the log screen still
shows for a moment).


## 36. Steam's pictures for the entry, from the player's own build (2026-11-13)

David's report: the Steam entry that Add to Steam makes has no artwork and
no description. His choice for the pictures: made from his own build, on
his machine, so no game pictures are in the repo or the release.

**No description:** a non-Steam entry has none to set. `shortcuts.vdf`
holds the name, the program, its folder, options, the icon, tags and a few
switches; Steam's page for a non-Steam game shows no store text.

**The pictures** (`runtime/host/art.c`, `vf4ft --art DIR`): the game runs
with no window and draws chosen vertical blanks of its attract sequence
with the GPU renderer at 4 x (2560 x 1920); each picture is cropped and
scaled to Steam's sizes (each output pixel the average, in linear light,
of the area it covers) and saved as a PNG.

| File | Size | Scene (vertical blank) | Crop, in 640 x 480 units |
|---|---|---|---|
| `capsule.png` | 600 x 900 | 5,300: the first demo's close-up of Akira | 118, 40, 240 x 360 |
| `icon.png` | 256 x 256 | 5,300 | 202, 32, 150 x 150 |
| `wide.png` | 920 x 430 | 12,050: the title over its flames | 0, 80, 640 x 299.1 |
| `hero.png` | 1920 x 620 | 12,050 | 0, 124, 640 x 206.6 |

- The crops keep clear of what every frame of the attract sequence has
  over the picture: FREE PLAY at the top left, PRESS START BUTTON (it
  blinks) at the bottom, VER.B and the card icons in the bottom corners,
  and the fighter's name lettering at the right of the close-up.
- The run: default settings with sound, the cards and the network off,
  unpaced, the backup not loaded or saved, nothing of the player's read
  or written. Its clock is the machine's own (it starts from a fixed time
  and runs on emulated cycles) and a blank cabinet gets no input, so the
  vertical blanks show the same scenes on every machine. Two runs gave the
  same pictures pixel for pixel, and so did the Linux build and the
  Windows build under plain Wine. It takes about 25 s here.
- No window: the GPU device is made without claiming one, so it also runs
  with `SDL_VIDEO_DRIVER=offscreen` and under Wine with no display. Without
  the GPU renderer (no Vulkan) it ends with code 3 and makes nothing.
- `--art-frames LIST` (`2400,3000-6000/300`) saves whole pictures of those
  vertical blanks instead, for choosing scenes. The attract sequence of the
  art run: the warning screen from about 2,484, the maker's logo 3,085,
  the first demo's close-ups 3,469 to 7,100, the ranking 7,546 to 11,000,
  the title over its flames 11,170 to 12,100, the second demo from 12,517.

**Steam's side** (`tools/steam_shortcut.py add EXE --art DIR`): the
pictures go into each Steam user's `config/grid/`, named by the entry's
appid as Steam names a non-Steam game's custom artwork: `<appid>p.png`
(the library's capsule), `<appid>.png` (the wide capsule),
`<appid>_hero.png` (the banner behind the game's page),
`<appid>_logo.png` (a clear 16 x 16 picture: the banner already shows the
title, and without a logo Steam writes the name over it) and
`<appid>_icon.png`, which the entry's `icon` field names (the host's path
under Wine). A different picture already there is backed up beside itself
(`*.vf4ft-backup-<time>`); the same one is left ("unchanged"). `--dry-run`
now says "would add", "would update" and "(dry run)" on what it would
write.

**The launcher's Add to Steam** is now three steps:
1. The tool's check, `add --dry-run`: with Steam running it ends with 3
   and the close-Steam note comes up before any pictures are made;
   nothing is written either way.
2. The pictures, into `game/steam-art/` beside the game: the game runs
   with `--art` and `--log steam-art/art.log` (so `vf4ft.log`, the
   player's last game, is kept). The Play page says "Making Steam's
   pictures from your build (about half a minute)…". Pictures already
   there are used when all four are newer than the game's program, so a
   second press, or Try again, takes a second. The run is ended after
   5 minutes. Only the game's `art:` lines (and anything it says before
   its log opens, such as an older game's "unknown option --art") go into
   `launcher.log`.
3. The entry, with `--art` when the pictures were made. When they were
   not, the entry goes in without them, and the status line and the added
   note say so ("launcher.log says why").

The native Linux launcher adds the Windows build, which it cannot run, so
its entry gets pictures only if `steam-art/` already has fresh ones.

**Checked** (never on Steam's own files: copies through `--steam` or a
stand-in `HOME`; Steam hidden from the tool by `bwrap --unshare-pid`
where it had to be closed; sound off and SDL's dummy audio):
- The art run, Linux build offscreen and Windows build under plain Wine
  11.21 with no display: Vulkan, 4 files in about 25 s, identical between
  the two builds and between runs. The four pictures at 1:1.
- The tool, Linux and Windows under Wine, with `--art`: the five grid
  files in each user's folder, the `icon` field naming the picture, a
  second run "unchanged" for all five; the two copies' grid folders the
  same byte for byte (the `shortcuts.vdf` files differ only in the icon's
  folder).
- The Windows launcher under plain Wine in a headless KWin (`--do steam
  --exit`): Steam running, the check ends with 3, the close-Steam note,
  no pictures made; an older game without `--art`, its "unknown option"
  line in `launcher.log` and the entry without pictures; this game, the
  pictures made in 25 s and the entry with the five grid files; pressed
  again, the pictures reused and Steam's all "unchanged", in a second.
- The Play page's new lines and the added note with and without pictures,
  offscreen at 1:1 (`--shot steam-art`, `steam-done`, `steam-done-plain`,
  `steam-added`, `steam-added-plain`).

Not tried: a real Steam showing the pictures (David's to see: Add to Steam
with Steam closed, then start Steam), Windows itself, and the Deck.

## 37. Card profiles, the launcher's Cards tab and the card editor (2026-11-13)

David's request: a "character profile" per player, as the cabinet's card
was; a default profile to start from, new ones made from it, and each
assigned to a player. His choices: the editor in the launcher first, a
ready card with every item as the default, and assigning from both the
launcher and the game's menu.

**A card** is a 128-byte file in `user/cards/NAME.card` (`runtime/host/
cards.h`): the card unit's 32 words, words 0 to 4 the card's own (word 1
its number), then the 108 bytes the game packs its record into (vf4lib's
libcard, `card_pack` and `card_unpack` in the decompilation). Final
Tuned's record is its format 4: CRC-16 (polynomial 0x8005, from 0, high
bit first) over the header (bytes 2 to 23, stored little-endian at 0 and
1), over the body (26 to 83, at 24 and 25), and the header again as bytes
84 to 107. `card1` and `card2` in `settings.txt` name each player's card
(`p1` and `p2` by default then; `none` by default since section 42); a name is 1 to 31 characters,
without `/ \ : * ? " < > | #`, a space or dot at either end, or a Windows
device name.

**A card is one character's:** with a registered card in (flags 0x11),
the select screen puts the cursor on the card's character and the player
cannot move it (`select.c`). So a profile is a character: the editor's
Character row is what David's "character profile" is.

**The game's menu** has a Cards tab (Player 1 card, Player 2 card, and the
card units' switch, moved from the Game tab). Left and Right go through
None and, by name, the profiles in `user/cards`, the players' cards not
made yet, and the player's own new card (`p1`, `p2`) while no file has
that name, shown "p2 (new)". Choosing the other player's card swaps the
two, as the launcher's Player 1 and Player 2 do. The rows change with the
card units off too (for the next start), as the launcher's do. As the
menu closes, a changed card is swapped in on the machine's thread: the
card in the unit is written to its file, then taken out (`hw_card_take`,
a `recomp` addition to VF4debug's card unit model). A card the game holds
(its lock, while a game uses it) leaves once the game lets it go, as the
cabinet's unit ejects a card when it is unlocked; the new one goes in
then. One card is in one unit at a time: a card the other unit still holds
(a swap) goes in once that unit has let it go (`cards_swap`).

The panel on the right shows the chosen card's details, as the launcher's
Cards tab does: character and rank, ring name, versus record, games left
and items, or what the card is (not made yet, never used, an older format, a
file that is not a card). Both draw the text with `card_view_text`
(`cards.c`); the launcher adds its Edit hints. The menu reads the card's
file again when the row changes and every half second. So that the file
holds the game's latest, the machine thread writes the units' cards to their
files (`cards_save`, tmp file and rename) as the menu pauses it, not only
every 600 vblanks: opened mid-fight, the menu shows the game just begun
(399 games left on a 400-game card).

*Versus record (2026-11-13):* David won a fight against the computer and
his card still had no wins. The game counts only versus matches there:
`game_judge_init` calls `card_match_result_record` (`all_win`,
`all_lose` and the rank change) after a match between two players, and
after one against the computer (`play_style` 1) only moves on to the next
opponent. Games left (`use_count`, 500 on a new card) goes down by one
when a game starts on the card, not on a win streak. The text said "0
wins, 0 losses", so it reads "Versus record: 0 wins, 0 losses" now.

*Fixed after 2a54824:* the first menu left the other player's card out of
each list, so two cards could not be swapped, and offered a card not made
yet only while it was the player's: with player 2's `p2` (not made yet)
set to None, player 2 had only None to choose, `p1` being player 1's.
Its two-step way round (None for one player, then that card for the
other) could also put a card held by a game into the second unit before
the first let it go; `cards_swap` now waits for that.

**The launcher's Cards tab** (Settings): the list of cards, a player's
card with no file yet among them ("not made yet": the game makes it at
the first game), and the chosen card's character, rank, ring name,
versus record, games left and items. Its bar:
- **Player 1, Player 2:** the card becomes that player's (lit when it is);
  pressed on the player's own card, the player has none. Giving a player
  the other player's card swaps the two. Saved with the settings (Done).
- **Edit:** the card editor, for a format 4 card whose checks hold. A card
  never used, or one not made yet, opens as a ready card (below) and is
  written as one on Save; a card never used keeps its number.
- **New:** a ready card named Card (Card 2, ...), then its editor.
- **Copy:** the card under "NAME copy", with a new card number.
- **Rename:** a note with a text field (31 bytes of UTF-8 at most, the
  name rules above); the file is renamed and the settings follow it.
- **Delete:** a note that asks first (Cancel is chosen); the file moves to
  `user/cards/Deleted/NAME YYYY-MM-DD HHMMSS.card`, and a player whose card
  it was has none.

Up and Down choose the card, Left and Right the button, A or Enter
presses it; Delete or X is Delete, Q and E the tabs, B or Esc Done. The
mouse chooses a card by its row (only chooses: the bar acts).

**The ready card** (`card_starter`): words 0 to 4 as a new card's, a card
number 0x2005xxxx (random; the unit model's own cards are 0x2004xxxx),
flags 0x33 (a card made at a cabinet: the game adds 0x80 at its first
game), Akira, 10th kyu, every item (`item_have` all 64 bits), 500 games
(`use_count`), `ftf` 0x02 (format 4), a random 15-bit `play_id`, the ring
name PLAYER, the rest 0, then the CRCs.

**The card editor** (its own page, `P_EDIT`; tabs Card, Costume 1,
Costume 2). It reads and writes the record's fields as `card_unpack` and
`card_pack` place them (`card_fields_get`, `card_fields_put`):

| Row | Where | Values |
|---|---|---|
| Ring name | bytes 44 to 63 | ASCII 0x20 to 0x7E, up to 20 (the card's own is EUC-JP: a name outside ASCII shows as "?" and is kept unless typed anew) |
| Character | byte 3, low four bits | the 16 but Dural (13, which the game refuses on a card) |
| Rank | byte 4 | 0 to 19: 10th kyu to 10th dan; a VF.NET title rank above is shown and only left downward. A new rank zeroes exp and point (28, 29) |
| Games left | byte 17, byte 18 bit 0 | 1, 10, 50, 100 to 500 by 100 (Left and Right go to the next of these) |
| Items owned | bytes 9 to 16 | all, none, or the card's own |
| Color, parts 1 to 4 | per costume: four bits each in bytes 30 and 31 (32, 33); the fifth in byte 42 bits 4 to 7 (costume 2: byte 77 bits 5 to 7 and byte 78 bit 2) | 0 its own, 1 to 19 |
| Hair color, Eye color | byte 64: costume 1 the high four bits, costume 2 the low, each hair << 2 \| eyes | hair 0 to 2, eyes 0 to 3 |
| Item slots 1 to 4 | bytes 34 to 37 (38 to 41) | 0 empty, 1 to 199 |

Changed colors or item slots clear the costume's `color_flag` or
`equip_flag` (byte 77 bits 0 to 3), as the game's own `costume_colors_set`
and `item_equip` do; VF.NET's mode sets them. Nothing checks a slot's item
against the items owned or the character: the renderer (`rob_item.c`)
skips a number past the character's list or one with nothing for that
costume, and the slot locks (byte 42's low bits, cursed items) are left
as they are. Save writes the card only when something changed (or it is
a new ready card); Cancel leaves it; Backspace or Y undoes all changes.

The notes that take a name (Rename, Ring name) open with the old name
chosen (shaded): the first key typed replaces it, the first Backspace
clears it. Enter is OK, Esc Cancel; on a gamepad X is Backspace, and on a
Steam Deck Steam + X shows the keyboard (the note says so).

`--shot-keys LIST` (the launcher's `--shot`, for tests): SDL key names,
`text:WORDS` typed (`_` a space) and `click:X,Y` in the page's units, one
after another before the picture. The pages `--shot cards` and, with
keys, the notes and the editor.

**Checked** (sound off and SDL's dummy audio; the game in a headless
KWin; the launcher offscreen, and the Windows launcher under plain Wine in
a headless KWin with its own Xwayland; test folders only):
- The launcher, driven by `--shot-keys`: New (a ready card written, its
  editor opened), the editor's every row, Save (only what changed is
  written; the CRCs and the header copy hold, by an independent Python
  reader), Cancel, Copy (a new number), Rename (the file and the settings
  follow), Delete (asks; the file in `Deleted/`; a not-made card only
  unassigned), Player 1 and Player 2 (assign, unassign, swap), Done
  saving `card1` and `card2`, a click on a row and on the bar. The pages
  at 1:1: the tab with cards, a non-card file (red, Edit and Copy off),
  a not-made card, the notes and both editor tabs.
- The Windows launcher under Wine: New, the ring name, Save, Rename and
  Delete with Windows paths. (Its pictures there come back black through
  Wine's Direct3D read-back; with SDL's software renderer they match the
  Linux build's.)
- The game with launcher-made cards as player 1's, a game started from
  the attract screen: the ready card (Akira, PLAYER, 10th kyu, 499 games
  after it) and an edited one (Kage-Maru picked by the card at the select
  screen, its typed ring name, 9th kyu, 399 games); the game wrote both back with its
  own changes (flags 0x80, exp, play_id) and kept the name, items, colors
  and worn item. The same run with that card's costume fields cleared:
  the select screens identical; in the fight Kage's hood is blue instead
  of green, and the worn item's strap is gone.
- The menu's swap (earlier in this section's work): a card swapped while
  no game holds it goes in at once; one held by a game (`--unpaced`, a
  game started) leaves once the game lets it go, 4,714 vertical blanks
  later, written with the game's changes.

Not tried: Windows itself, Proton and the Deck (its keyboard in the
notes), a cabinet card from VF.NET with a Japanese ring name, and which
item numbers each character has (the editor goes by number; David's
earlier choice).

## 38. The sky above a victory pose: the guard band (2026-11-13)

David saw the top of the screen black at the end of a fight, while the
winner did a victory pose. Reproduced with `--press` (player 1 mashing
kick and punch, so that player 1 wins; `--press` now takes 8,192 presses
for that): on the stage with the orange cloud sky, player 1's victory
pose (vertical blanks 10,362 to 10,662 of that run) showed the background
plane's black above a ragged line, on the GPU renderer and the software
one alike, so not a renderer's fault. The other stages' poses were whole.

**Cause:** `view.c`'s `tri_fill` left out any triangle whose corners lie
more than a million pixels from the screen (the rasterizers' floats no
longer place an edge to the pixel out there). The low camera looks up
past the sky dome's huge triangles; cut at the near plane (z 0.1, 1/w 10)
close to the eye, their corners land 1.5 million pixels above the screen.
A temporary count in `tri_fill` found 13 such triangles every frame of
the pose, each overlapping the screen from its top down to rows 142 to
341: the black area.

**Fix:** such a triangle is cut in view space at four planes 4,096 pixels
outside the target (`guard_cut`; a plane is linear in X, Y, Z because x =
cx + sx X / Z), as the near plane already cut it, and its pieces drawn as
a fan. A triangle within the bound takes the old path, so the pictures
are the same everywhere else. The volumes' triangles (`vol_tri_view`)
get the same cut: cutting keeps every pixel's count inside the band, so a
shadow volume's in-or-out stays right on the screen.

A strip's room for triangles was two per triangle (the near plane's cut);
the band can make six. Room for six each grew the game's peak memory by
105 to 110 MB, almost all of it never written, so each triangle keeps its
two and the strip shares a spare of four per triangle, at most 64
(`tri_spare`): 9 to 12 MB more than before.

Checked (`--shot`, the same run on both renderers, the old `view.c`
built beside the new one): the two victory pose pictures show the orange
sky up to the top; six other moments (a fight, five other stages' poses)
are pixel for pixel the old ones on both renderers; the spare draws the
same 16 pictures as room for six. Not checked: real hardware or another
emulator's picture of that pose (the sky above the band is the dome's
own, drawn from the game's data).

## 39. Wide screens without stretching: 16:10, 16:9 and 21:9 (2026-11-13)

David asked for wide screen options with no stretch, for the Steam Deck
OLED (16:10) and 21:9 ultrawide screens too. The Picture setting
(settings.txt `picture`, the menu's and the launcher's Picture row) is now
4:3, 16:10, 16:9, 21:9 or stretch. A wide choice draws the game's screen
pass 768, 854 or 1,146 pixels wide (480 tall; 21:9's is a 3440 x 1440
screen's shape) instead of 640, at the GPU renderer's scale (2,292 x 960
for 21:9 at 2x).

**How (`view.c`, `view_host_wide`).** The game still makes its 640 x 480
scene: the projection, the clip rectangles and the 2D are its own. After
transform, lighting and the translucent sort, `scene_shift` moves the
pass's triangles, modifier volumes and clip rectangles right by half the
extra width, so the game's screen lies in the middle and the camera's view
goes on at each side: what lay left of x = 0 (kept now, `keep_left`) or
right of 640 was there all along and is drawn. A clip rectangle that
reaches the game's left or right edge reaches the target's. The
background plane moves with them (`bg_plane`). Passes into textures and
any pass not of the whole 640 x 480 screen are unchanged. The buffers are
1,280 wide (`FBW`); the game's own passes keep their old 1,024 limit
(`PASS_W`).

**The 2D.** The HUD, the writing and the 2D screens (the warning, the
character select) stay 4:3 in the middle, unstretched; a 2D screen has
black sides. A 2D triangle that goes from side to side of the game's
screen and looks the same stretched sideways moves its corners at the
screen's edges out to the target's (`tex_flat_across`: no texture, or v
does not change along x and each row of the texels it covers is one
colour). That covers the stage intros' black bars (without it they were
black only over the middle 640, with the 3D above and below them at the
sides), the full-screen black and white flashes, and the rankings' lines.
A logged run (a temporary count in `emit`, removed) found these to be all
the screen-wide 2D triangles of a fight and of the attract mode; the
others that cross the screen are zooming pictures much wider than it,
left alone. The game's own fades (the white-out between rounds, the
title's white) are in its 3D and fill the sides already.

**The window.** The picture keeps its shape: the window letterboxes or
pillarboxes it as before, at the picture's width over 480 rather than
4:3. A wide picture's window started 720 tall at its shape (1,719 x 720
for 21:9); section 40 gave the window its own setting. The setting changes at once from the menu; the next frame is
drawn at the new width. Steam's pictures (`--art`) are always 4:3.

**Checked** (`--shot`, the headless KWin, the same mashing run as
section 38): 4:3 is pixel for pixel the pictures before this change on
both renderers (five moments). At 16:9 the middle 640 of the wide picture
is the 4:3 picture to rounding (at most 0.35% of its pixels differ, almost
all by one level, since the triangles sit at other x). Fights, victory
poses, stage intros, round changes, the title, the rankings and the
attract mode's demos at 16:10, 16:9 and 21:9 on both renderers: the sides
are the stage, with nothing missing in the shots. The menu's Picture row
changed from 4:3 to 16:10 during a fight: the next picture was 16:10, in
the 4:3 window with bars above and below. Not checked: objects the
game culls at its own 4:3 view's edges popping in at the sides of 21:9
while the camera moves (none seen in the shots taken), and a real Deck or
ultrawide monitor.

## 40. Quit quits; Window size and Internal resolution (2026-11-13)

David's report: with the Resolution row at 4x and a 16:9 picture, the
window stayed 1,281 x 720 on his 4K screen. The row was the GPU
renderer's size (3,416 x 1,920 there, scaled down to the window), and the
window had a fixed size of its own (section 39). He asked for both: an
internal resolution and a window size. And the game's Quit brought the
launcher back, though the menu has Open the launcher for that.

**Quit.** The launcher closed with the game only when it had started the
game directly (section 29); otherwise any end of the game showed it
again. Now it closes whenever the game it started ends with code 0 (Quit,
or the game's window closed). It still shows itself for Open the launcher
(code 64), an update asked for in the game's menu, and a game that ended
with an error. The Start the game directly switch now only keeps the
launcher's window hidden at the start.

**Internal resolution** is the Resolution row renamed (settings.txt
`scale` as before): the size the GPU renderer draws at, before the
picture is scaled to the window or the screen. It applies at the next
start, as before.

**Window size** (settings.txt `window`, the menu's and the launcher's
Picture tab) is the window's height in pixels at the picture's shape:
Auto, 480, 720, 960, 1080, 1440, 1920 or 2160 (the renderer's four sizes,
a window pixel for each of its pixels, and the screens'; more since
section 43). The width is the
screen's shape, not the picture's width over 480 (`settings_window_width`:
16:9 at 1080 is 1,920, where 854 x 480 gives 1,922; 21:9 is 3440 x
1440's). In the room (`window_room`), that is, the screen's usable area
less the window's frame:

- **Auto** is the largest listed height that fits in nine tenths of the
  room (the launcher's own first size uses the same 90%, section 28): 720
  on a 1080 screen with a title bar (960 with none, as on Wayland here),
  1440 on a 4K one.
- **The list.** The rows offer only the heights that fit the room and are
  no smaller than the smallest window (640 x 400 window units, the
  menu's, times the pixel density). A height from a hand edit or another
  screen is made smaller at its shape to fit, or larger to the smallest
  window, and the row shows the size the window gets.
- **Pixels.** The room and the sizes are pixels: window units times
  SDL's pixel density (2 on a Wayland screen at 200%; 1 on Windows and
  X11, whose units are pixels). The window is created at the size worked
  out on the primary screen, with its pixels taken as units; then,
  synced, it is set again from the screen it opened on, in its own pixel
  density, with its frame (`SDL_GetWindowBordersSize`) known: KWin keeps
  an X11 window's title bar on the screen, so 1,080 lines on a 1080 screen
  came out 1,052 tall before the frame was counted.
- **Changes in the menu** apply at once, in the middle of the screen: a
  new Window size, or a Picture of another shape (the window takes the
  new shape at the setting's size; 4:3 and stretch are one shape). A
  maximized window is restored first. In fullscreen the change waits
  until fullscreen ends (F11, or the Fullscreen row). Dragging the
  window's edges still resizes it; the setting is unchanged.

The launcher's rows work out the room from its own window: its screen,
its pixel density and its frame (the game's window has the same kind of
frame).

**Checked** in a headless KWin (`kwin_wayland --virtual`, Xwayland on),
the game's own `--keys` driving the menu and `shot=` taking the window as
shown, in pixels:

| Case | Wayland 1920 x 1080 | X11 1920 x 1080 | Wayland 3840 x 2160 at 200% |
|---|---|---|---|
| Auto, 16:9 | 1708 x 960 | 1280 x 720 (room 1920 x 1052) | 3414 x 1920 |
| Auto, 4:3 | 1280 x 960 | 960 x 720 | 2560 x 1920 |
| 1080, 4:3 | 1440 x 1080 | 1402 x 1052 (the title bar's 28) | 1440 x 1080 |
| 2160, 16:9 | 1920 x 1080 | 1870 x 1052 | |
| 720 to 960 in the menu, 16:9 | 1280 x 720, then 1708 x 960 | the same | 1422 x 800 (the smallest window), then 1708 x 960 |
| then Picture 21:9 | 1920 x 803 (made smaller) | the same | 2294 x 960 |
| fullscreen; 720 to 960 in the menu; F11; F11 | 1920 x 1080, stays; 1280 x 960; 1920 x 1080 | the same | |
| the menu's list, Right from Auto | 480, 720, 960, 1080 | 480, 720, 960, Auto (1080 does not fit) | 960, 1080, 1440, 1920 |

The launcher with a stand-in game ending with a chosen code, offscreen,
the switch off and on: 0 closed it 1.2 s after the start, 64 and 3 left it
open (3 with the error on the Play page). Its Picture tab, drawn with
`--shot`, has the six rows.

The Windows game under plain Wine (an X11 window through Xwayland, the
same KWin at 1920 x 1080): Wine gives its own frame, so the room was
1912 x 1046. Auto at 16:9 was 1280 x 720; 720 to 960 in the menu 1708 x
960, then 21:9 1912 x 800; fullscreen, the change, F11 and F11 again 1920
x 1080, 1280 x 960, 1920 x 1080; the list Auto, 480, 720, 960, Auto. 1080
at 4:3 asked for 1394 x 1046 and got 1394 x 1052: Wine's frame is not
the window manager's, so a window the room's full height can be a few
lines off its shape (bars of 3 lines).

Not tried: a real Windows PC, Proton under Steam, the Deck, a screen with
a panel (the usable area less it), and windows moved between screens of
different scales (the menu works out the room each time it opens).

## 41. Region (2026-11-14)

David asked how to change the game's region, and for a region choice in
the menu.

**Where the region comes from.** On a cabinet it is the BIOS's (the
board's BIOS chip), not a setting: the game's test menu has none. As the
game starts, `board_start` (`sdk/libam/nlam.c` in the decompilation) asks
the BIOS for its board settings (service 0x28: the country, two flags, DIP
switch 1), `board_start_core` keeps the country in `boot_country` and
`board_settings`, and `system_param_country_init`
(`vf4lib/libbase/src/system_common.c`) reads it from there: 0 Japan, 1 USA,
2 Export, 3 Korea, 4 Australia (anything else stops the game with an
assertion). The BIOS stand-in (`hle_boot.c`) answers with `cfg.country`,
which was always 0 in the window game; only the headless runner had
`--country`.

**What it changes:** every caller of `system_param_country_get`, and the
coin settings.

| Region | What the game does |
| --- | --- |
| Japan (0) | `warning_init` shows the WARNING screen ("THIS GAME IS TO BE USED ONLY IN JAPAN ...", 600 vertical blanks) after power on; `win_streak_draw` uses the Japanese layouts and digit sprites; `shun_drink_tint_apply` tints Shun Di by his drink count |
| the others | `warning_init` goes straight to the attract loop; win streaks in the overseas layouts with ASCII digits; no tint |
| USA (1) | `sega_free` puts RATING_DISP before the SEGA logo: "Parental Advisory Warning / This game is classified LIFE-LIKE VIOLENCE-MILD" for 180 vertical blanks, from the attract loop's second round (`warning_init` sets `advertise_sub6_skip` for the first) |
| Australia (4) | `australia_notice_draw` asks for "AUSTRALIAN VERSION" at (16, 36) on the title screen (see below) |
| coins | `coin_config_unpack`'s maximum credit: 24 for USA and Export, 9 for the others (arcade coins only) |

The game's text is otherwise the same in every region.

**The setting:** `region = japan | usa | export | korea | australia` in
`settings.txt` (default japan); the Region row on the game menu's Game tab
and the launcher's Settings Game tab. `game_machine_region` puts the
country in `cfg.country` before `game_machine_init` (which clears `cfg`:
it keeps its own copy) and, from the frame function, before
`game_machine_restart`: the game asks for the board settings again as it
starts, so Restart the game applies it as it does Credits and Network
(`start_cfg.region`). The EEPROM's system area holds no country
(`board_config_unpack` takes it as an argument), so a backup made in one
region works in another, and `eeprom_first_start` (read in a Japanese run)
serves every region: FREE PLAY showed in each from a blank backup.

**Checked** in a headless KWin (`scratchpad region/`: the Linux game,
paced, shots by `--keys`; a blank backup, the network off, so the
start-up's fast-forward ends near vertical blank 3000):

- Japan: the warning at 3000 and 3060, then the SEGA logo from 3120 and
  AM2 from 3300.
- USA and Australia: no warning, the attract demo already at 3000;
  identical to each other up to the second round (3,441,618,428 and
  3,441,618,422 instructions at 3780), the title screen at 6500 in both;
  then USA's rating screen from about 6570 to 6745 and its SEGA logo
  after it, 180 vertical blanks behind Australia's.
- Restart: Japan at the start; at 3100 the menu, Region to USA, Restart
  the game ("vertical blank 3109: the game starts again (the menu's
  Restart), region USA"): after the new start-up the SEGA and AM2 logos
  with no warning; the same Restart with Japan left on shows the warning
  again (5400, 5700).
- The menu's row (Japan to USA, the dot for later, `region = usa` saved),
  the launcher's Game tab (offscreen `--shot settings`). Linux, win64 and
  the builder (its own source list) compile.

**Australia's label is not seen.** Australia's and USA's title pictures at
6500 are identical (no pixel differs). The decompilation's notes for
`australia_notice_draw` record a harness run as region 4 that queued the
18 sprites of the text on every title frame, so the game asks for them;
whether they are hidden on the white title screen by the game itself (z
0.008, the font's colour) or lost in our renderer is open, and needs a
capture from a real board or a closer look at the sprite list. The help
text does not promise the label.

Not tried: win streaks and Shun's tint (they need play), Export and Korea
runs (the same branches as Australia's but the coins), the test program's
system information, the Windows game.

## 42. No card by default; the card editor in the game's menu (2026-11-14)

David's request: no card given to a player by default ("When a card is
assigned to you, you can only play with the character on the card. Good
for tournament mode, bad for arcade mode"), and the card editable from the
game's menu the same way as from the launcher.

**No card by default.** `settings_defaults` leaves `card1` and `card2`
empty (`none`); they were `p1` and `p2` (section 37), so every first game
made a card that then held its player to one character (`select.c`: with a
registered card in, flags 0x11, the select screen puts the cursor on the
card's character and the player cannot move it). A `settings.txt` already
written keeps what it says: the launcher and the game write every setting,
so an install from 0.5.2 to 0.5.4 still names `p1` (and `p2`) until a
player picks None. The game menu's list still offers a new `p1` or `p2`
("p1 (new)") while no card has that name. The help texts (Card units,
Player 1 card, Player 2 card, the launcher's Cards tab) say a card holds
its player to its character and None leaves every character free.

**The editor's rows are shared.** The launcher's editor (section 37) moved
into `runtime/ui/card_editor.c` (`ced_*`): the tabs, the rows and their
descriptions, Left and Right on each row, the ring name's text in and
out, Undo, and which cards can be edited (`ced_editable`: a Final Tuned
card whose checks hold, a card never used, or one not made yet). The
launcher's page and the game's menu each draw it and take its keys their
own way; the launcher's behaviour is unchanged.

**In the game's menu,** the Cards tab has **Edit player 1's card** and
**Edit player 2's card** under each player's card row (greyed, with the
reason, for None or a card that cannot be edited). Choosing one puts the
editor in the tabs' place: "Card: NAME" at the top, its tabs Card, Costume
1 and Costume 2 (Q and E, LB and RB), its rows (Up and Down, Left and
Right, held to repeat) and the chosen row's description, Undo on Backspace
or Y, and Save and Cancel at its foot (clickable). Esc, B, Guide or a right
click outside the rows save and go back to the Cards tab; Esc again closes
the menu. A or Enter on the ring name opens its note: the old name chosen
(the first letter typed replaces it), up to 20 ASCII letters, numbers,
spaces and marks, Backspace or X a letter less, Left and Right between OK
and Cancel, Enter or A for the chosen one, Esc or B cancel. The window
turns on SDL's text input only while the note is up (`menu_typing`), and
sends the letters (`SDL_EVENT_TEXT_INPUT`, `menu_text`); only Enter, Esc,
Backspace, Left, Right and Tab are commands then. The caret does not blink:
the menu is drawn again only when something changes.

**When a game is using a card.** The game keeps its own copy of a card
for the whole of a game, and writes that copy back over the card twice
(the decompilation's `card_mode.c`): it reads the card when the player
presses Start (`card_mode_entry_read`, which copies the record into the
side's card image, `card_record_to_image`), writes the image back as the
character select ends (`card_mode_select_write`: a play taken off, a new
code) and only then locks the unit (0xd9, two frames later); after the
match it reads the card again but writes its image over what it read
(`card_mode_result_write`, `card_image_to_record`: header and body
whole, the name, the character and the costumes too), then opens the lock
(0xda). A change put on the card anywhere in that span is lost. The first
build of this section took "the lock is on" for "in use" and lost an edit
saved at the character select: the run's request log showed the reads at
vertical blanks 3333 and 3347 with the lock off, the edit written at
3884, the select write at 4483 (its name back to PLAYER) and the lock at
4495. So the unit model now keeps `read_open`, set by a read made with
the lock off and cleared when the lock opens or the card leaves, and
`hw_card_held` is "a card is in, and it was read with the lock off or the
lock is on". A read with no lock after it (a card the game refuses, the
test menu's card pages) holds the card until it leaves the unit; an edit
to it then waits for a swap or the machine's stop.

**Where a Save goes.** The machine is paused while the menu is open, and
a card in a unit is the game's, as above. So the menu does not write the
file. A Save hands main.c the fields that changed
(`card_edit_add`: each field the editor shows, as the change from the card
as opened to the card as saved), kept per card (`pend`, under
`shared.lock`); a second Save of the same card adds to it. On the machine
thread, every vertical blank after the menu's pause (`cards_edits`, before
`cards_swap`):

- a card in no unit: its file, with the fields put in (`card_edit_apply`,
  `card_fields_put`), written through a tmp file and a rename;
- a card in a unit the game is not using: written to its file first
  (`cards_save`), taken out, its file changed, put back in, all in the
  same vertical blank, so the game never sees the unit empty;
- a card a game is using (`hw_card_held`): it stays in, and the edit waits
  (the log says so once). When the game lets go (the lock opens at the end
  of the game), the card as the game left it gets the fields as above, so
  the game's own changes (the record, games left, rank progress) stay and
  the editor's go on top;
- a card never used or not made yet (`fresh`): a ready card (section 37's
  `card_starter`, keeping a card never used's number) with the fields in;
- as the machine stops (Quit, Open the launcher, the window closed), every
  edit still waiting goes onto the card's last state (`cards_edits(1)`).

The menu shows a card with its waiting edit in (`card_peek`): the Cards
tab's details and the editor opened again. The editor's note and the Edit
row say when a game is using the card. Two `recomp` additions to the card
unit (`hw_card.c`, ORIGIN.md): `hw_card_held` (above; also for that
note) and `hw_card_last` (the card in the unit, or the one that left it
last, as it left), which `game_machine_card_get` now uses, so the game's
last write to a card that leaves as the lock opens reaches its file even
when the lock opens in the same vertical blank (before, `cards_save` found
the unit empty then).

`--keys text:WORDS` (the game's test hook) types WORDS ("_" a space) as
the keyboard's text; the list now takes 8,192 characters and 96 items.

Checked (Linux game in a headless KWin, the launcher offscreen, the
Windows game and launcher built, the player's builder compiled):

- no card settings: both units stay empty, and the Cards tab shows None
  with both Edit rows off and the reason;
- at the attract loop (card1 = Ace): the editor opened as "Card: Ace";
  Character Right to Sarah, the ring name typed (Tester), a costume color
  changed; the Cards tab showed the waiting edit; the file was written as
  the menu closed, its checks good, and the select screen showed Tester,
  Sarah held;
- in a game (Start pressed, the editor opened at the character select):
  the held note showed, the name saved as Held waited 8,147 vertical
  blanks and went on as the lock opened after the match; the card kept
  the game's own changes (499 games left, the experience point, the new
  code) with the name Held;
- a gamepad (A opens, Right, Y undoes, B saves) and the mouse (a click on
  a value's right half, Cancel saves nothing), with the hints in the
  device's glyphs;
- a card not made yet (card1 = p1): Edit, Save as it is: a ready card
  inserted;
- the launcher's editor: the same picture as before, a new card edited
  and saved with good checks;
- the Windows game: David played the test copy (built from these
  sources) on 2026-11-14 with no problems.

Not checked: a Steam Deck's keyboard, an edit waiting when the machine
stops.

## 43. More window sizes (2026-11-14)

David asked why Window size offered so few sizes, with lists of the usual
16:9, 4:3 and 21:9 screens' resolutions. The list (section 40) was the GPU
renderer's four heights and the screens' 720, 1080 and 2160; nothing else
limited it. Two things were missing: the heights of the other usual
screens, and the ultrawide screens' own widths. A 21:9 window was always
3440 x 1440's shape (43:18), but 2560 x 1080 and 5120 x 2160 are 64:27 and
3840 x 1600 is 12:5, so a "1080" 21:9 window was 2,580 wide (too wide for
the screen it is named after, so made smaller to fit).

**The heights** (`settings_window_heights`, 16 of them): 480, 576, 600,
720, 768, 800, 900, 960, 1050, 1080, 1200, 1440, 1536, 1600, 1920 and
2160. At the picture's shape they give every size of David's lists: 16:9
854 x 480, 1024 x 576, 1280 x 720, 1366 x 768, 1600 x 900, 1920 x 1080,
2560 x 1440, 3840 x 2160; 4:3 640 x 480, 800 x 600, 1024 x 768, 1280 x
960, 1400 x 1050, 1440 x 1080, 1600 x 1200, 1920 x 1440, 2048 x 1536; and
the 16:10 screens' 1280 x 800 (a Steam Deck), 1440 x 900, 1680 x 1050,
1920 x 1200 and 2560 x 1600. The rows still offer only what fits the
screen, and settings.txt's note lists them all.

**21:9** (`settings_window_width`) is the ultrawide screens' own width at
their heights: 2560 at 1080, 3840 at 1600, 5120 at 2160 (3440 at 1440
already was); other heights keep 43:18. The game's 21:9 picture itself
is unchanged (1146 x 480, section 39), so in a 2560 x 1080 window it has
4-pixel bars above and below, as it has in fullscreen on that screen.

**Auto** is unchanged in its rule (the largest listed height in nine
tenths of the room), so with more heights it comes closer to that: 1600 x
900 at 16:9 on a 1920 x 1080 screen with a title bar (X11, Windows),
where it was 1280 x 720; 1708 x 960 without one, as before.

The Window size row's description names a few sizes; its "1920 x 1080"
keeps together with no-break spaces (U+00A0, which the fonts have and
`ui_wrap` does not break at).

Checked: every size above computed by `settings_window_width` (a scratch
program on settings.c; the lists the rows offer on 1920 x 1080 with and
without a title bar, 1366 x 768, 2560 x 1080, 4K at 200% and 1280 x
800); the Linux game in a headless KWin: the menu from Auto, Right five
times, 854 x 480, 1024 x 576, 1068 x 600, 1280 x 720, 1366 x 768, saved
as `window = 768`; Auto at 16:9 1708 x 960 (Wayland) and 1600 x 900
(X11, room 1920 x 1052); 21:9 at 1080 on a 2560 x 1080 screen a 2560 x
1080 window, the picture 2560 x 1072.

## 44. The Steam Deck: its screen's shape and the OLED's refresh rate (2026-11-14)

David asked for the game to know the Steam Deck and the Deck OLED and to
fill their screens on its own. Fullscreen's Auto already went fullscreen
on a Deck (Steam's `SteamDeck=1`, section 20); what was missing was the
picture's shape and, on the OLED, the refresh rate.

**Which Deck** (`settings_steam_deck`, settings.c): the firmware's
product name, `/sys/class/dmi/id/product_name`: Valve's board names,
`Jupiter` for the LCD Deck and `Galileo` for the OLED. The Windows game
runs under Proton on a Deck, where the Linux files are on Wine's Z: drive,
so it reads `Z:\sys\class\dmi\id\product_name`, but only under Wine
(`WINEPREFIX` or `STEAM_COMPAT_DATA_PATH` set): on a real Windows PC a Z:
drive is something else. Neither name and `SteamDeck=1`: a Deck whose model
is not known. It is read once; the log says `device: Steam Deck OLED`.

**The picture** (`settings_defaults_here`): both Decks are 1280 x 800,
16:10, so on a Deck the default Picture is 16:10 (section 39) instead of
4:3, and with Fullscreen's Auto the game fills the screen with no bars.
It is only the default: a settings.txt that says a picture keeps it. The
launcher's Settings page uses the same defaults; the Steam artwork
(`--art`, section 36) stays 4:3. A Deck known only by `SteamDeck=1` gets
16:10 too (other handhelds running Steam's Deck interface may be 16:9;
their player changes Picture once).

**The refresh rate.** The game shows 60 pictures a second, paced by its
own timer (`FRAME_NS`), and the window presents with vsync. A screen
refreshing at a multiple of 60 (60, 120, 240) shows each picture for
the same number of refreshes; at 90 Hz (the OLED's top rate) pictures are
shown for one or two refreshes in turn, at 144 Hz for two or three, and
the motion judders. The game cannot set the screen's rate: on a Deck it
is Steam's (the Quick Access button, Performance, Use per-game profile,
Refresh rate), elsewhere the system's. So it says so, once:

- `settings_refresh_uneven(hz)`: the rate is known and not within 1 Hz of
  a multiple of 60 (59.94 and 120 are even; 90, 75, 144 and 165 are not).
- Two seconds into the picture (after the Ko-fi notice, section 26, when
  that shows), main.c reads the window's screen's rate
  (`SDL_GetCurrentDisplayMode`, logged as `screen: 144.00 Hz`). When it
  is uneven, or on a Deck OLED whatever it reports (the per-game rate is
  Steam's, and what gamescope reports to the game is not known to follow
  it), the menu opens as a notice (`MENU_NOTICE_REFRESH`): "Set the
  screen to 60 Hz", the steps, and OK.
- `user/refresh-notice` says what it was shown for, `Steam Deck OLED` or
  the rate (`144 Hz`), on its first line: it shows again only for
  another screen. Test runs (`--frames`, `--keys` and so on) show it only
  with `--refresh-notice N` and write nothing; `--deck-product FILE` and
  `--screen-hz HZ` stand in for the firmware's name and the rate.
- The Picture tab's Fullscreen row says the same in its note, every
  time, while it applies.

Checked, in a headless KWin (its virtual screen reports 60 Hz whatever
its configuration says, hence `--screen-hz`): Linux and the Windows game
under Wine with no settings.txt and a `Galileo` product file: `device:
Steam Deck OLED`, fullscreen, 16:10, the 1280 x 800 screen filled, the
notice at 60 Hz, then the Fullscreen row's note; `Jupiter` at 60 Hz:
16:10 and no notice; a settings.txt with `picture = 4:3` on the OLED
keeps 4:3; another product name: 4:3 and no device line (also the
Windows game reading this PC's own name through Z:, and Wine's `type`
of that file shows the name); 144 and 90 Hz: the notice naming the rate;
two normal starts at 144 Hz: the first shows it and writes `144 Hz`, the
second logs that it was shown and shows nothing. The launcher with
`SteamDeck=1` shows Picture 16:10, without it 4:3. Not yet on a real Deck.

## 45. Two players on two PCs: the plan, and the same machine everywhere (2026-11-14)

David asked about two things: a VF.NET server like Flycast's, and two
players playing each other on two PCs, in phases: a LAN first, then a
private online server, then public matchmaking. The two-PC play comes
first; VF.NET follows (it starts with reading what the event lists of the
`power_on` reply do in the game).

**Why it is netplay.** The cabinet never linked to another for a versus
game: VS is two players on one board (`play_style` 2), and the network
board talks only to the operator's servers (section 5, `hw_dimm_net.c`).
So two PCs each run the whole machine, the same run, and send each other
only the players' controls for each vertical blank. That needs the
machine to be the same run on both, bit for bit, from the same start.

**The phases.**

1. LAN: one PC hosts (player 1), the other joins (player 2), by address
   or found on the network; inputs a vertical blank or two ahead (on a
   LAN, not felt); UDP, each packet repeating the last inputs.
2. Private online: the same over the internet, through a forwarded port
   or a small relay David runs; rollback (the machine saved each frame,
   frames run again when a late input differs), which needs a fast
   in-memory snapshot (`snapshot_save_mem` goes through a file). The
   machine takes about 3 ms a frame on a desktop, so it fits there; the
   Steam Deck (an estimated 10 to 12 ms) stays on input delay.
3. Public matchmaking: a lobby, a queue by ping, a relay so players never
   see each other's addresses. A decision for later.

**Cards live with their player** (David's rule). A card's file stays on
its player's PC, or later on the VF.NET server we build, which can be its
home so it follows the player to any PC. For a game both PCs need both
cards, since both run both card units: at the start each PC sends its own
player's card, and the other player's card is kept in memory only. When
the game writes the cards back, each PC saves its own player's card to its
own `user/cards/` and drops the other; the two PCs compare the bytes first
(a difference means the run has split, and the card is not written). The
cabinet (`backup.bin`, coins, region) is the host's for the session; the
joining PC's own backup is not touched.

**Phase 0, the ground under all of it:**

- The same game on both PCs: each builds its own from its own set, so the
  two compare a fingerprint of the built game and refuse to play when it
  differs.
- The same run on both (below: done for the processor's arithmetic).
- Inputs by vertical blank number: the machine now takes whatever the
  window holds at its vertical blank (`on_frame`).
- The same start: the host's backup, settings and both cards; the boot
  is the same run from there.
- A fingerprint of the machine every so often over the link, so a split
  is seen at once.
- The game's menu pauses both PCs (or stays shut); no card editing in a
  session.

**The same machine everywhere: FSCA.** The machine was already
deterministic (section 16: the clock is fixed at `hw.rtc_base`, the sound
chip runs a fixed budget a sample), and compiled arithmetic is IEEE on
every PC (`-msse2 -ffp-contract=off`, no `-march`). One instruction asked
the host: FSCA computed its sine and cosine with the C library's `sin` and
`cos` (`op_fsca`, and the interpreter's `fsca`). Two PCs with different
math libraries could part by a last bit, and a fighting game's positions
come from those angles. Evidence first: all 65,536 angles dumped with the
same expression gave identical bits from glibc (64-bit and 32-bit, with or
without gcc's merged `sincos`), Wine's ucrtbase and Proton's. Real
Windows (Microsoft's ucrtbase) could not be tried here, and a library that
does not reset the rounding mode inside `sin` would differ again. So FSCA
now reads a table (`runtime/cpu/fsca.h`): the sine for a quarter turn
(16,385 floats, `fsca_table.inc`, written by `tools/make_fsca_table.py`,
which checks every angle) and three constants (the double-precision
angles for a half turn and a quarter and three quarters are not exactly
pi, pi/2 and 3pi/2, so those results are tiny numbers, not 0). The
values are the ones glibc gave, so nothing changed on Linux.

The game's rounding mode: the old code rounded `sin`'s double to float in
the host's current mode, which follows the guest's FPSCR.RM. A temporary
count in the game (20,000 vertical blanks from a blank cabinet, through a
game) found 11,264,419 FSCAs, none with RM = 1 (round to zero), none whose
table result differed from the old way. The lockstep tests' random states
do set RM = 1, where VF4debug's interpreter gives a step nearer zero on
some angles (8 failures, each one step): `tests/lockstep/lockstep.c` now
takes such an interpreter result as the table's and counts it; any other
difference still fails.

**The fingerprint** (`game_machine_hash`, `--hash-every N`): main RAM and
the processor's registers in four lanes of FNV-1a over 64-bit words, about
1.1 ms (it will be the session's split check). Logged as `state: vertical
blank N: HASH (ms)`.

Checked: the Linux game (gcc) and the Windows game (llvm-mingw clang)
under Wine, the same blank cabinet, settings and presses (a game started,
a second player joining), unpaced to vertical blank 20,000: all 40
fingerprints the same, the same instruction count (19,584,134,467), in
GAME MODE at the end; the same fingerprints as the Linux run before the
temporary count. The lockstep tests: 56,294 instruction cases x 32
states and the routine runs, 1,804,564 checks, none failed (129 FSCA
results in round-to-zero states taken from the table).

## 46. Two PCs on a LAN: the link, the start, the checks (2026-11-14)

Phase 0 and phase 1 of section 45: two players on two PCs on one network,
each PC running the whole machine, with only the players' controls going
between them.

**Using it.** In the launcher, the Play page has a **Two PCs** row under
Play. Its note has **Host** (this PC is player 1 and waits; its game shows
this PC's address and port) and **Join** with the host's address typed in
(this PC is player 2). The address joined last is kept in
`launcher-two-pc.txt` beside the launcher. From the command line:
`--host [PORT]`, `--join ADDRESS[:PORT]` (a name, IPv4, or IPv6 in
`[ ]`), and `--delay N` (vertical blanks of input delay, 1 to 30, the
host's counts; 2 by default). The port is UDP 29404 unless one is given.

**Why a request file.** On Linux, the Windows build's Play goes through
Steam (`steam_shortcut.py play`, section 34), which passes no arguments.
So the launcher writes the game's `user/two-pc.txt` ("host" or
"join ADDRESS", then the time in seconds) and starts the game as Play
does. The game reads it as it starts, however it was started, removes it,
and does not use one older than two minutes. The launcher removes it again
if the game did not start (the program failed to start, or Steam's
`play` failed). `user/` is already among the install folder's names.

**The link** (`runtime/host/udp.c`, `session.c`): UDP, one socket, the
system's own sockets (Windows' `ws2_32`). Every datagram starts with
`VF4N`, the protocol's version, its kind and the session's number; the
kinds are in `session.c`'s first comment. The joining PC sends HELLO
every 250 ms for up to a minute. A thread sends INPUT every 10 ms whether
or not the game is running a frame, so a PC in its menu still answers;
15 seconds with nothing heard ends the session. Each INPUT repeats every
input the other PC has not yet acknowledged, so a lost datagram costs
nothing but the next one's 10 ms.

**The same game.** HELLO carries the build: the version and, for a game
made by the builder, a fingerprint of the sources it was built from
(`build.py`'s `source_id`: SHA-256 over `runtime/`, `translator/` and
`tools/`, the first 12 hex digits). The host refuses another build with a
note on the joining PC's screen. The game's code itself comes from the
player's set, whose image and programs the builder checks by SHA-1, so
the sources and the set make the same game. A CMake development build says
"development build (sources unknown)" and two of those play each other.

**The same start.** The host's cabinet is the session's: coins, region,
network, card units, and its `backup.bin`, which goes over in 1,024-byte
chunks with a bitmap of what has come, sent again until all are there.
The joining PC's own `backup.bin` and settings are not read or written.
The sound chip runs on both PCs whatever the sound setting, since it is
part of the machine; what is heard still follows each PC's setting.

**Inputs by vertical blank.** The game's start-up step (it runs unpaced
until the game first shows its warning screen) takes no inputs on either
PC, so neither waits for the other there. As it ends, each PC stamps its
player's controls (the player 1 controls and keys on both PCs, which play
as player 1 or 2 by the role) for the vertical blank `delay` ahead, and a
vertical blank runs only when both players' inputs for it are here. A wait
longer than 300 ms is shown on the screen ("Waiting for player 2's
PC."). A coin is an input like a button.

**The checks.** Every 60 vertical blanks each PC takes a fingerprint:
`game_machine_hash` (section 45) and both card units' bytes. INPUT carries
the newest four. The same vertical blank with two different fingerprints
ends the session at once: "The two games went apart at vertical blank
N." The log says at the end how many agreed.

**Cards** (section 45's rule). Each PC sends its own player's card: the
card file named for player 1 in its settings (none, a card never used, or
its 128 bytes). The other player's card is kept in memory only. At each
check this PC's player's card is kept as it was, and a card is written to
`user/cards/` only once a check both PCs agreed on has come: the card as
it was at that check, and only this PC's player's. So a card is never
written from a run that split. The Cards menu cannot change cards in a
session (no card choosing, no editor).

**The menu.** Opening the game's menu on one PC stops both: the other
shows "Player 1 has the game's menu open." Test mode, Restart and the card
rows are not in the menu during a session. The Ko-fi and refresh-rate
notices do not show in a session (they would pause both PCs).

**The end.** Quitting sends BYE; the other PC plays the inputs it already
has, then says "Player N left the game." and keeps its window, with the
game stopped, until it is closed. The cabinet's backup is saved by the
host only. A game whose session never started (no answer, refused, the
other PC gone during the start) keeps its window with the reason and exits
with code 4; the launcher shows itself again.

**What was checked** (in a headless KWin, both games over loopback,
unpaced, with fingerprints every 500 vertical blanks):

- Linux host and Linux joining PC, a game started and a second player
  joining, to vertical blank 20,000: all 40 fingerprints the same on both
  PCs, and the same as one game alone with both players' presses moved on
  by the input delay; 333 checks agreed; the same instruction count.
- Linux host, the Windows build joining under Wine: the same, and the same
  fingerprints as the Linux pair.
- Cards on, both players with a card, the host's menu open for about four
  seconds mid-game: the joining PC waited and said so; each PC wrote only
  its own player's card, each the same bytes as the game alone wrote.
- Both games started by `user/two-pc.txt` only: both read it, removed it
  and played (50 checks agreed).
- The launcher, offscreen (`--shot two-pc`, `--shot-keys`): Join with an
  address typed and Host each wrote the request and started the game,
  which found it; a game that could not start left no request.

**Not yet:** finding the host on the network without typing its address;
a host listening on IPv6 (it listens on IPv4; a joining PC can reach an
IPv6 host only once it does); the internet (phase 2: a forwarded port or a
relay, and rollback); Restart in a session; a test on two real PCs and on
real Windows (its firewall's question when the game first hosts).

## 47. Two PCs: the host's side, and the host playing on alone (2026-11-15)

Two things after section 46: the host chooses whether it is player 1 or
player 2, and when the joining player quits, the host keeps playing as a
game for one. Also: the launcher asks for an address only once Join is
chosen.

**The launcher.** Two PCs' note now has **Host as player 1**, **Host as
player 2**, **Join…** and **Cancel**, and no address field. Join… opens
a second page, "Join a host", with the address field (the address last
joined is filled in), **Join** and **Back**; Escape or a gamepad's back
button goes back to the first page. The last choice is the note's first
button next time (and drawn as the main one). `launcher-two-pc.txt` holds
the address last joined, then that choice (`host`, `host 2` or `join`);
one written by 0.5.6, the address alone, still reads. The note is drawn
in the launcher's 960 x 600 units like every page, so it scales with the
screen as they do (checked at 1x and 2x). The Play page's line says "One
PC hosts, as player 1 or 2; the other joins it over your network as the
other player."

**The request and the command line.** `user/two-pc.txt` may say
`host 2` as well as `host` (and `host 1`). From the command line,
`--host-player 2` with `--host`. The joining PC needs nothing new: it is
the player the host is not.

**The protocol.** WELCOME gains one byte after the backup flag: which
player the host is. HELLO is unchanged, and a joining PC of another build
is refused before any WELCOME (section 46's build check), so the
protocol's version stays 1. Each PC's own player is still its settings'
card1 and its player-1 controls, whichever player it is in the game;
`session_start` now has `own` (this PC's player's card, both roles) and
`player[2]` by player, and the session puts each card in its unit.

**Playing on alone.** When the session ends on the host for any reason
but its own window closing (the other PC left, stopped answering for 15
seconds, or the two runs went apart), the host's machine does not stop:

- It saves the backup, ends the session (the network thread stops, BYE
  is sent) and logs "vertical blank N: the two-PC game is over; this PC's
  game goes on alone (player N)".
- From that vertical blank on, its player's controls go straight in, with
  no input delay, as that player; the other player's are none, so the
  game goes on as a game with one player (the other's character stands
  until the round ends; then it is the arcade game against the computer).
- Its note shows why, then "Your game goes on: you play alone." for 8
  seconds.
- Its own card is written as it is, every 60 vertical blanks when it
  changed and once more when the machine stops. The other player's card
  is still never written: it stays in memory only.
- The menu stays as it was in the session (no test mode, Restart or card
  rows) until the game is closed.

On the joining PC nothing changes: its game stops with the reason, now
followed by "The game stopped.", and keeps its window until it is closed.
The window titles say which player each PC is: the host's from the start,
"(player N, hosting)"; the joining PC's "(joining)" until the start is
agreed, then "(player N)".

**The launcher follows Fullscreen.** It used to open as a window whatever
the game's Fullscreen setting said. Now its window is fullscreen when the
game's would be (`settings_fullscreen_wanted`, shared with the game: on,
or auto on a Steam Deck or in Big Picture), read from the install
folder's `user/settings.txt`. The window is made hidden and shown once the
install folder is known, already fullscreen, so it never opens as a window
first. It follows the setting again when Settings closes and when the game
ends (the game's menu changes it too). Its page stays 960 x 600 units,
letterboxed into the screen. `--shot` pictures keep the page's own size.
Window size stays the game's alone: the launcher's window is sized for its
page and the screen's scale.

**What was checked** (headless KWin, both games over loopback, unpaced,
cards on, fingerprints every 500 vertical blanks):

- The host as player 2 through `user/two-pc.txt` ("host 2") on both PCs:
  the joining PC was welcomed as player 1; all 40 fingerprints the same on
  both PCs and the same as one game alone with the host's presses as player
  2's and the other's as player 1's, moved on by the input delay; each PC's
  card the same bytes as the game alone wrote.
- The host as player 2 by `--host-player 2`, the joining game quitting at
  vertical blank 8,000: the host went on alone from vertical blank 8,003
  to 20,000, and all 40 of its fingerprints were the same as one game alone
  with the host's presses moved on by the input delay before 8,003 and not
  after. Its card was written alone at 12,000, 12,060 and 18,240 and was
  the same bytes as the game alone's; the other player's card was not
  written on the host.
- The launcher, offscreen with `--shot-keys`: Host as player 1, Host as
  player 2, Join… then an address, Join… then Back then Host as player 2,
  a remembered `host 2` (the address last joined kept), a remembered
  `join` (Join's page filled in), a 0.5.6 file (the address alone), and
  Join's page left empty then Back and Cancel: each asked the stand-in
  game for what was chosen, or started nothing.
- The launcher's window, as KWin measured it on a 1280 x 800 screen:
  fullscreen for Fullscreen on (Wayland, X11, and the Windows launcher
  under Proton), for auto with `SteamDeck=1`, and after a game that turned
  Fullscreen on and gave the launcher back (exit 64); a 960 x 600 window
  for off, auto, and no settings file.

## 48. The whole machine saved and resumed in another process (2026-11-15)

The first step towards a second PC joining a game that is already going
(the host playing alone, section 47): the joining PC cannot play the game
from power on to catch up, so it has to take the machine as it is. This
section shows that a machine saved part way through, loaded into a fresh
process, goes on as the same run, on Linux and on Windows alike.

**What a snapshot holds.** VF4debug's snapshots (runtime/machine/machine.c,
used by its controller's sandbox): the processor, the hardware registers,
the count of vertical blanks, the scheduled events (by their routines'
registration order), the models' own state (maple, card units, graphics,
sound and the sound chip, the cartridge, the DIMM board, the server
stand-in, the inputs, the BIOS stand-in), and every memory area page by
page (a page of zeros as one byte). A load checks the whole file before it
changes anything, and refuses one whose parts have other sizes (another
build).

**What only the translated game keeps,** and how a resumed run gets it
(`rt_native_load`, runtime/native/rt_native.c):

- The blocks the game changed in its own code (section on code the game
  changes): until the run starts, memory holds the program's own bytes, so
  the load compares memory before and after and marks every block a
  changed byte falls in, or whose compiled-in pool word it changes, as a
  store would have. The game changes one block (0c149980) as it starts;
  the resumed run marks it the same way.
- The host's rounding mode for translated code: loaded from FPSCR as the
  run starts (`rt_fpscr_loaded`); the interpreter sets its own before
  every instruction.
- The C frames of the routines the run was inside: there are none in the
  new process. The run goes on from the saved PC with `rt_drive` in a loop:
  it runs to a return at its level, and the next turn goes on in the code
  after the call. That code runs on the interpreter (a routine's C cannot
  start part way), but everything it calls runs as C again. The game's
  main loop never returns, so its own few instructions stay interpreted:
  measured, the resumed run was about 5 to 10% slower (5.0 to 5.6 ms a
  vertical blank, unpaced, where the run from power on took 4.7 to 5.1). A
  Restart or test mode goes back to the normal run from the power-on
  state.
- The test program's run is never saved (its tables are the other
  program's); the save says it failed.
- The exception depth dispatch.c counts is only the observer's, and the
  renderer's state (the scenes it has taken from the graphics chip's
  lists) is only what is drawn: neither is in a snapshot, and neither
  changes the run. The resumed window has no picture until the game draws
  its next frame.

**Where it is taken.** At the end of the frame hook (`host_frame`), once
the vertical blank's inputs have gone in: `game_machine_snapshot_ask` from
the frame function, `game_machine_snapshot_get` at the next one. A resumed
process makes its machine as usual (`game_machine_init`, the user's backup
and cards), then `game_machine_resume`; its first frame function is at the
vertical blank after the snapshot's. For tests: `--snapshot-at N FILE` and
`--resume FILE`.

**What was checked** (headless KWin, unpaced, sound chip on, cards off,
presses: a game started at 3,400, a challenger at 9,000, fingerprints
every 500 vertical blanks):

- Run A from power on to vertical blank 20,000, saved at the end of 6,000
  (the middle of the first round of stage 1). Run B, a new process with its
  own empty user folder, resumed from that file with the same presses: all
  28 fingerprints from 6,500 to 20,000 the same as A's, the same count of
  instructions at the end (19,584,134,467), the same mode, and the pictures
  at 6,060 and 9,000 the same to the pixel.
- The same file resumed by the Windows build under Wine: the same 28
  fingerprints, instruction count and pictures. The parts of a snapshot
  have the same sizes in both builds (no `long`, no pointers).
- A `--shot` asked for at 6,001 or 6,002 in the resumed run never
  completes: the window has no picture yet, and the machine waits for the
  picture to be saved. That is the test option's own limit (a shot needs a
  picture already shown); the shots were moved to 6,060 and 9,000.

**Its size.** 64,019,064 bytes at vertical blank 6,000: main RAM 27.8 MB
of 32 (its program code among it), the ELAN geometry chip's RAM 20.1 MB,
sound RAM 7.6 MB, the graphics chip's memory 6.0 MB, the models' state
2.4 MB (1.7 of it the DIMM board's). zstd at level 1 makes 22.4 MB of it. A game booted to
the title screen with no presses (vertical blank 5,900) already has 9,742 of
those pages the same, so what differs is 24.1 MB, and `zstd --patch-from`
that booted state makes 5.6 MB.

**Next: joining a game that is going.** The plan this proves possible:
the host, playing alone, keeps listening; a joining PC of the same build
says hello; at the end of a vertical blank the host puts the joining
player's card in that player's empty unit and takes a snapshot, then waits
(its note: the other player is joining) while the snapshot goes over; the
joining PC resumes it, and both go on in lockstep from the next vertical
blank with the input delay, as section 46's start does. The size above
decides how the snapshot is sent: whole and compressed, or as the
difference from a game the joining PC boots by itself while it asks.

## 49. Joining a game going on (2026-11-15)

Once the joining player of a two-PC game leaves, the host's game goes on
alone (section 47). Now another PC can join that game, as the player who
left, and play on from where the game is. It joins the same way as at the
start (Two PCs, Join…, the host's address); the host's note while it
plays alone says that address.

**What goes over.** The joining PC needs the host's whole machine (section
48), about 64 MB. Most of it is the same as a machine that has only just
started: the program's code, the graphics, the sound driver and its
samples. So both PCs keep a snapshot of their machine at the end of the
session's start-up, the reference, and the host sends only how its
machine now differs from it:

- The host takes its reference at the first vertical blank after the
  start-up (`session_reference` in runtime/host/main.c) and keeps it all
  session.
- The joining PC makes the same reference itself. The host answers its
  hello with the session's start as it was (a LATE in place of the
  WELCOME): the cabinet's settings, the input delay, the backup as it was
  then, both players' cards as they were then, and the reference's
  vertical blank, size and fingerprint. The joining PC starts the game
  from that, with no inputs, the boot fast-forwarded as always: the same
  run, so the same machine at the same vertical blank (2,387 with a blank
  backup and the network off). Both cards of that start are kept in
  memory only; its own card file is not touched.
- Once its copy is at the reference, it says so with the copy's
  fingerprint (READY). The host compares it with its own. When they are
  not the same (they always were in the tests), the difference is made
  from nothing and is bigger, but the join still works.
- The difference: runtime/host/snap_patch.c, a format made for this
  program. The reference is indexed every 16 bytes; each position of the
  machine now is looked up there, and in what came before it; the longer
  match wins, and the rest are new bytes. It ends with the result's
  fingerprint, which the joining PC checks. 13.5 MB for a machine at
  vertical blank 10,386 (a game of stage 1 going on), 15 to 16 MB later in
  the arcade run; made in 0.3 to 0.5 s, applied in 12 to 24 ms. zstd on top
  saved only 15% (measured on section 48's pair of snapshots: 7.2 MB, 6.1
  with zstd), so it is not used.

**The join, step by step.** The host's machine thread does its part at the
vertical blanks (`late_host`):

1. A joining PC's copy is ready (`session_late_ready`).
2. The card unit of the joining player's side: the card of the player who
   left is taken out. If the game is still using it (that player left in
   the middle of a game), it leaves only once that game is over; the join
   waits, and both notes say so. Then the joining player's card goes in
   (a card never used gets its number from the machine, as at the start).
3. The machine is saved at the end of that vertical blank F.
4. At the next vertical blank the difference is made and handed to the
   session (`session_late_send`), and the session runs again from F + 1:
   inputs from F + 1 + the delay, fingerprints every 60 vertical blanks,
   exactly as section 46's start. The host's game runs its first
   `delay` vertical blanks with no inputs and then waits for the joining
   PC's, its note saying how much of the game has gone over.

The joining PC, at the vertical blank after its reference, waits for the
difference, makes the host's machine from it and loads it in the middle
of its own run (`rt_native_load` from the frame hook: the C stack is left
by `longjmp`, as a restart leaves it, and the changed code blocks are
found again from the bytes, section 48). Its next frame function is at F +
1, and its own card is now the one in its unit, written to its file as
the two PCs agree on it.

**The transfer.** 1,176-byte chunks (PATCH; 1,200 before section 50's
seals), with the vertical blank F,
the whole size and whether the card went in, so the first one to arrive
says where the session goes on. The joining PC acknowledges every 16
chunks and every 10 ms (all below a number, then a bit each for the next
4,096); the host keeps at most 512 chunks on their way, sends one again
when it is not acknowledged within 50 ms, and takes the joining PC's first
INPUT as saying it has all of it. Both sockets ask for 4 MiB of buffer
(udp.c), as a burst of 512 chunks is about 600 KB and Windows gives 64
KiB.

**After.** Each time the joining player leaves, the host is open again;
the reference stays the one from the session's start. A PC that stops
answering while it joins (15 s) or says it leaves is dropped and the host
stays open. A PC that tries to join while a session runs is told "A game
is going on there already"; while another is joining, "Another player is
joining there now". Another build is refused as at the start.

**What was checked** (headless KWin, unpaced, sound chip on, over
loopback; A hosts, B joins at the start and leaves at vertical blank
9,000; presses on every PC through the run; fingerprints every 500
vertical blanks in each log):

- Linux to Linux, cards off: C joined at 10,387; the difference, 13.5 MB in
  11,242 datagrams, came in 68 ms; 427 session fingerprints agreed up to
  36,000, and all 56 logged fingerprints of C (its own start-up from 500 to
  2,000, then the host's game from 10,500) are A's.
- Cards on, B leaving in the middle of a game: the join waited 10.7 s for
  that game to end, then C's card went in. C left at 27,000 in the middle
  of a game of its own and D joined (2.8 s wait for C's card). C's and D's
  cards were written to their own files as the PCs agreed on them; 246 and
  217 session fingerprints agreed; all logged fingerprints matched A's.
- The Windows build under Wine joining a Linux host: 426 session
  fingerprints agreed, all 56 logged ones matched. But the difference took
  1.9 s, and 7,159 of its 11,239 chunks had to be sent again: the joining
  socket's buffer was too small for the bursts over loopback. With the 4
  MiB asked for (the system gave 4 MiB under Wine, 8 natively), the same
  join took 150 ms with none sent again (Linux to Linux: 17 ms), and every
  fingerprint still agreed.
- A long game: the host played alone for an hour of the game's time
  (217,282 vertical blanks) before C joined. The difference was 15.4 MB
  (made in 307 ms; 12,829 datagrams in 25 ms, none sent again; applied in
  12 ms), C's replay to the reference took 2.5 s as always, and the whole
  join about 3 s; 79 session fingerprints agreed. The replay is only of
  the start-up, so it takes as long after an hour as after a minute, and
  the difference is the machine's changed memory, which does not grow
  with time (13 to 16 MB in every run). Nothing of it is kept on disk:
  the reference is in memory only (the joining PC frees its own once
  used), and vf4ft.log starts again at each start of the game (about 36
  KB of lines an hour while playing).

**Not done.** The host can only be joined once a session has been played:
a host that starts alone without waiting would need the reference taken
the same way, which is a small step from here.

## 50. The join code: only the PC told it joins (2026-11-15)

Until now any PC that could reach the host's port could join, and one
that wrote the joined PC's address on its datagrams could play as that
player. Now the host's game shows a six-digit join code beside its
address, and only a PC with that code joins; everything after is sealed
with keys only the two PCs have.

**The code.** Six digits from the system's random numbers (getrandom on
Linux, BCryptGenRandom on Windows; 32 random bits, taken only below
4,294,000,000 so each code is as likely). The host's note shows it while
it waits for the other player and while its player plays alone, and the
game's menu shows it in a row of its own, **Join code** (on the Game tab,
in a two-PC game only). Each code works once: a PC that joins with it uses
it up, and when that player leaves the host makes a new one. Three wrong
codes and it stops working until the host's player chooses **Join code**
in the menu for a new one; the note says a wrong code was typed and on
which PC.

A code that changed every 30 seconds was the first idea. It was not
taken: a code an attacker can guess at forever, three tries each 30
seconds, is weaker than one that stops after three, and with six digits
three tries are 3 in 1,000,000. A new code needs the host's player.

**Why no certificates.** A certificate says who is at the other end
because someone both ends trust signed it. Two people on one network have
no one to sign them; what they share is the code, read off one screen and
typed on the other. A password-authenticated key exchange makes keys from
just that. Certificates would come in with a server between PCs on the
internet, which this does not have.

**The exchange: CPace.** draft-irtf-cfrg-cpace-21, CPACE-X25519-SHA512.
Each PC makes a point on Curve25519 from the code (the generator) and a
random number of its own; each sends its point, and both make the same
key only if they used the same code. Nothing that goes over tells the
code: a PC that does not know it learns nothing it could test codes
against afterwards, so each guess takes an exchange with the host, which
counts it.

1. The joining PC sends HELLO: the build (first, as before), 16 random
   bytes (CPace's sid), its point Ya, and zeros up to the old HELLO's
   size.
2. The host answers CHALLENGE: its point Yb. The same HELLO again (the
   CHALLENGE lost) gets the same CHALLENGE; a newer HELLO replaces the
   try (one at a time). A point that gives the neutral element is not
   answered.
3. The joining PC makes the key and sends PROOF: its player (the card)
   sealed with it. Only now does anything depend on the key; had the host
   sent something sealed first, an attacker could take it away and test
   every code against it.
4. PROOF opens on the host: the code was right, the code is used up, and
   the host sends WELCOME (or LATE, section 49) sealed. It does not open:
   a wrong code, counted, and a REFUSE says so.

The code is CPace's PRS; its channel words (CI) are "VF4FT two PCs: " and
the build, so another build makes other keys too; ADa is "joining PC",
ADb "host". The key (ISK, 64 bytes) gives one key for each direction
(keyed BLAKE2b).

**Monocypher.** runtime/crypto holds Monocypher 4.0.3 (X25519, Elligator 2,
SHA-512, BLAKE2b, XChaCha20-Poly1305), unchanged from its release; see
ORIGIN.md. It is used under CC0 1.0, one of the two licenses it offers.
One difference from the draft is bridged in runtime/host/secure.c:
Monocypher's Elligator map reads 254 bits of its number (its
representatives keep the top two for padding), while CPace's
decodeUCoordinate keeps 255. The map depends only on the number's square,
so it is first reduced below p and, when 2^254 or more, replaced by p
minus it (in constant time). The draft's own vectors need this: their
generator's hash has bit 254 set.

**Sealed datagrams.** Every datagram after the exchange, both ways
(inputs, BYE, the backup's chunks, PATCH, their acknowledgements, LATE,
WELCOME, REFUSE): the header stays clear (its kind gets bit 7) and is
checked, an 8-byte counter follows (the nonce), then the body,
encrypted, and a 16-byte tag; 24 bytes more in all. A datagram that does
not open, or whose counter came before (a window of 2,048), is dropped
without an answer, and it does not move the window. PATCH's chunks are
1,176 bytes now, so a sealed one is 1,223 bytes: under the 1,232 an IPv6
path always carries in one piece.

**Other builds.** The HELLO keeps the build at the front and the old
size, so a 0.5.6 host refuses a new PC as another build (and says so),
and a new host refuses a 0.5.6 PC the same way.

**Where the code is typed.** The launcher's Join… now has a second page,
The join code (digits only; Join is pressed when there are six). The game
takes `--code CODE` with `--join`, and two-pc.txt's line is `join ADDRESS
CODE`; the launcher logs the request without the code, and the game wipes
it once read. The host logs each code it makes ("the join code is now
...") in its own vf4ft.log: the tests read it there, and it is no more
than its own screen shows.

**What PCs outside the session can make the host do.** Answer a HELLO of
the full size (a REFUSE or a CHALLENGE, both shorter than the HELLO, so a
forged sender's address gets no more than it sent; a shorter HELLO gets
nothing), and log a line about it: 20 such lines while one code stands,
then one saying the rest are not logged. Text that comes from another PC
(its build, a REFUSE's words) goes into the log and the notes as
printable ASCII only. They can still get in the way: a forged REFUSE
before the keys exist ends a join, and three wrong codes stop the code.
They cannot join or play as the player.

**Tests.** tests/secure (`make -C tests/secure test`, 56 checks): the
draft's vectors (B.1.1 to B.1.5: the generator, both points, the key from
both sides) and its low-order points (B.1.10: those that give the neutral
element are refused), the generator and the map against an independent
Python version (tests/secure/reference.py; 40 generators of other lengths
and 57 numbers, among them the edges of the reduction), the codes (each
digit as likely; what is taken as typed), and the seals (any byte
changed, a datagram again, out of order, the window's edges, the other
direction's key, keys forgotten). With the reduction taken out, 7 of them
fail. Over loopback (headless KWin, unpaced; the codes read from the
host's log):

- A hosts. X types a wrong code: "The host said no: The code is not
  right." B joins with the code, plays to vertical blank 5,000 and leaves;
  the host makes a new code. A script (poke.py) then sends from a port of
  its own: an old build's HELLO (refused as another build), a HELLO cut
  short (no answer), a point that gives the neutral element (no answer), a
  random point (a CHALLENGE) and a PROOF of random bytes (refused, the code
  not right), 300 datagrams of junk (answers only to its full-size HELLOs,
  REFUSEs), and 60 HELLOs of other builds with control bytes in their
  names (all answered; the log has 20 lines about PCs outside, one saying
  the rest are not logged, and no byte that is not printable). C then
  joins the game going on with the new code. All of B's and C's logged
  fingerprints are A's (10 and 31).
- Three wrong codes (X1 to X3; the third is told the code no longer
  works), then Y with the right one: refused, "Three wrong codes were
  typed...". The host's menu (Join code, chosen with `--keys`) makes a new
  code and C joins with it: 12.96 MB in 11,023 datagrams, 37 ms, none sent
  again; every fingerprint agrees.
- The Windows build under Wine as the PC that joins the game going on:
  12.04 MB in 132 ms, all 32 logged fingerprints A's.
- The Windows build under Wine as the host (its codes and its points from
  BCryptGenRandom), the same three wrong codes and new code: the same
  answers, and C (Linux) took 12.96 MB in 120 ms, none sent again; every
  fingerprint agrees.

**Not covered.** The host's machine, as it comes to a joining PC (section
49), is read with its layout checked to the last byte (sizes, the event
list, every model's state and memory area), but what the models' state
says is used as it comes: control.c takes its count of held buttons
from it and walks an array of 32 with it. A host whose code a player
typed could make the joining PC write outside that array. It is the
other player's PC, which the code says it is; before the game goes past
one network, each model's state should be checked when a snapshot is
read (counts, positions, indices) and a fuzzer run over snapshots.
When the host's REFUSE after a wrong code is lost, the joining PC sends
its PROOF again, which the host does not answer, and it says after 60 s
that the host did not take its code.

## 51. Window size in order; the GPU and the display (2026-11-15)

Three asks from playing: Window size did not go up and down in order
when stepped right and back left; and the Picture tab should choose the
GPU the GPU renderer draws on, and the display the game opens on. Both
new rows are on the Picture tab of the game's menu and of the launcher's
Settings, in this order: Fullscreen, **Display**, Window size, Picture,
Scaling, Renderer, **GPU**, Internal resolution.

**Window size was out of order.** Its list was Auto, then the sizes that
fit the screen, smallest first, and it wrapped. Auto's size is one of the
middle ones (the largest that leaves a tenth of the screen free: 1280 x
960 at 4:3 on a 1920 x 1080 screen), so Right from Auto went down to the
smallest, Left from the smallest jumped up to Auto and on to the largest,
and the listed size Auto stood for came again after it (Auto (1280 x
960), then 1280 x 960). A checker that steps from a start value right to
the end, left to the other end and right again, on 11 screens (1280 x
720 to 3840 x 2160, the Deck's 1280 x 800, 2560 x 1080, Wayland ones at
150% and 200%) with each of the 5 pictures and from 8 start values, found
all 440 walks out of order.

Now (options.c window_step):

- the sizes that fit, smallest first, with **Auto at its own size**: in
  the place of the listed size it gives, which is not offered again;
- **the ends do not wrap**: Left at the smallest and Right at the largest
  do nothing, and that arrow is drawn dim (ui_row's `ends`, UI_END_LEFT
  and UI_END_RIGHT; only this row sets them);
- a value not in the list (a hand edit in settings.txt, a size kept from
  another screen, a listed height that is Auto's size here) steps from
  the window it gives to the next strictly smaller or larger one, so no
  size shows twice;
- a click on the left half of the value box steps left, in the game's
  menu and in the launcher: without the wrap, clicks (which step right)
  could not come back.

A wrapping list jumps from the largest window to the smallest, which is
the complaint again in another shape; a list of sizes stops at its ends,
as Volume does. The other choice rows still wrap: their values have no
order. The checker now finds 0 of the 440 walks out of order, and in the
game (headless KWin, 1920 x 1080, 4:3) the window went 640 x 480, 768 x
576, 800 x 600, 960 x 720, 1024 x 768, 1068 x 800, 1200 x 900, Auto (1280 x
960), 1400 x 1050, 1440 x 1080 and back, presses past either end changing
nothing.

**GPU: what SDL gives.** SDL 3.4.20's SDL_CreateGPUDevice takes a driver
name, not a device, and no property or hint names one. Its Vulkan backend
ranks the devices: a separate card, then one in the processor, then
virtual, then CPU, the one with more memory first among equals.
SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN puts the processor's
first. SDL_PROP_GPU_DEVICE_NAME_STRING says which device opened. On this
PC Auto is the RTX 5070 Ti and low power the Radeon 8060S.

**GPU: the choice.** Auto (the default) is SDL's choice, as before. The
GPUs are listed by devices.c gpus_list. It opens Vulkan's loader itself
(vulkan-1.dll or libvulkan.so.1, or SDL_HINT_VULKAN_LIBRARY's) and makes
a bare instance. It keeps the separate, processor and virtual devices
(llvmpipe and other CPU ones are left out) by their Vulkan names, cleaned
of `#` and control characters (settings.txt's comment mark), with one
entry per name. The game lists them only when it needs them: a GPU chosen
at start, or the menu open. The launcher lists them the first time its
Picture tab is drawn, since listing wakes every GPU. A chosen GPU opens
so (main.c gpu_device_open):

1. SDL's choice with low power preferred for a GPU in the processor, not
   preferred for any other; the device that opened is checked by name.
2. Not that one (two separate cards, say): VK_LOADER_DEVICE_ID_FILTER set
   to its PCI device id while SDL opens it, then put back, and checked
   again. On loader 1.4.363 the device id alone works, while vendor and
   device together match nothing. It works on Linux and Windows but not
   under Wine, where the Windows program's environment does not reach the
   host's loader (winevulkan calls that).
3. Still not: Auto for the run, logged ("cannot be had").
4. Not there at the start (an external GPU unplugged): Auto for the run,
   logged. The name stays in settings.txt and is used again once the GPU
   is back.
5. Opened, but it cannot show in the game's window: Auto for the run,
   logged ("cannot show the window"). A failed claim used to quit the
   game, so a GPU choice could have stopped the game from starting at
   all. Found in testing: Mesa's X11 swapchain on the Radeon fails
   (vkCreateSwapchainKHR VK_ERROR_INITIALIZATION_FAILED) when the X
   server draws on the NVIDIA card, natively and under Wine alike. The
   headless KWin of the tests renders on the NVIDIA, and its Xwayland
   with it (glxinfo says so), and that X server does not take the
   Radeon's pictures. On Wayland the same Radeon draws. On this PC's own
   desktop KWin draws on the Radeon, so the case is other PCs': an X11 or
   Proton game on a PC whose X server is on the card not chosen.

The properties path sets SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN to
false. It defaults to true, and the first version opened the chosen GPU
in debug mode (it looked for validation layers) while Auto did not.

The row shows Auto or the name, and "NAME (not found)" while a missing
one is chosen; the description's note has the whole name, which the row
cuts short. It shows only for the GPU renderer (greyed, with "The
Software renderer does not use it." for Software). It applies at the
next start (the menu's dot), and the menu's note says which GPU draws
now, or that the chosen one could not be used and which one draws
instead. Log: "GPU 1: NVIDIA GeForce RTX 5070 Ti (discrete, PCI
10de:2c05)", "GPU: NAME, as the settings ask (...)" or "GPU: NAME (Auto:
SDL's choice)", "renderer: GPU, W x H, on NAME".

**Display.** The display the game opens on, windowed or fullscreen.
Auto (the default) leaves it to the system, as before.

- Names: SDL_GetDisplayName, cleaned as the GPU's are; with no name,
  "Display N". Of two with the same name, the second (in SDL's order) is
  "NAME (2)", and so on.
- At the start the window is made at the center of the chosen display
  (SDL_WINDOWPOS_CENTERED_DISPLAY) and sized for that screen's room
  (Window size Auto). Fullscreen opens there.
- In the menu it applies at once. A window goes to the center of that
  display at its Window size there; a fullscreen one goes to that
  display. Auto leaves it where it is.
- F11 and Alt+Enter go fullscreen on the chosen display (fullscreen_to).
- Not connected (at the start, or unplugged): Auto, logged. The name stays
  in settings.txt and the row shows "NAME (not connected)". Displays that
  come or go update the menu's list while it is open.
- **Wayland:** a program cannot place its windows (SDL: "wayland cannot
  position non-popup windows"), so a window goes where the compositor
  puts it, and only fullscreen goes to the chosen display (SDL keeps the
  display asked for and fullscreens there). The log and the description
  say so. X11, Windows and Wine place the window.
- Names depend on the video driver. Wayland gives the output's
  description ("LG Electronics LG TV SSCR2"), X11 the EDID name with the
  size in inches ('LG TV SSCR2 55"'; KWin's virtual outputs come out as
  'Virtual-1 169093200"'), and Windows (and Wine: "Display2") its own.
  The launcher and the game use SDL's default driver, so on one desktop
  they agree; a name saved under another driver is not connected (Auto,
  logged). No loose matching: two displays alike would be mixed up.
- The window's moves are logged ("window: now on NAME", once a move;
  "window: fullscreen", "fullscreen off"). While the menu is open, Window
  size's sizes follow the screen the window is on.

**settings.txt:** `gpu = auto` or a GPU's name, `display = auto` or a
display's name, as the rows list them (case does not matter).

**Files.** runtime/host/devices.c and devices.h list the GPUs and
displays for the game and the launcher (in both CMake targets and
build.py's units). The rows are in options.c, the menu's notes and live
Display in menu.c and main.c, and the launcher's in launcher.c. Its
renderer and window code are unchanged.

**Tested** in headless KWin (kwin_wayland --virtual, one or two 1920 x
1080 outputs side by side, Xwayland on), natively and with the Windows
build under Wine 11 (a scratch prefix with Wine's audio driver off), and
the launcher's Picture tab with --shot offscreen:

| Case | Result |
|---|---|
| Window size stepped both ways, game and launcher | in order, ends dim and still |
| Display chosen at start, Wayland, windowed | the compositor places it; F11 fullscreen on the chosen display, back on leaving |
| Display chosen, fullscreen at start, Wayland | fullscreen on the chosen display |
| Display chosen at start, X11 and Wine | opens centered on it; F11 fullscreen there |
| Display changed in the menu, X11 and Wine | the window moves at once; F11 there |
| Display changed in the menu, Wayland | logged as not placed; F11 there |
| Display not connected | Auto, logged, kept; row "(not connected)" |
| GPU: Radeon by name (Wayland) | low power: the Radeon draws |
| GPU: RTX 5070 Ti by name, then Auto in the menu | drawn on it; saved as auto, the dot and the note |
| GPU not there | Auto, logged, kept; note says which draws |
| GPU: Radeon on X11 / under Wine (X server on the NVIDIA) | cannot show the window: Auto, logged, the game runs |
| GPU list under Wine | both GPUs, through winevulkan |

Not tested: the loader filter (step 2) on two separate cards of one kind,
which this PC does not have; a real monitor unplugged while the game runs
(the virtual outputs cannot be).
