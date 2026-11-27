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

**Menus go by label, the game by place** (asked for by the maintainer). The game's
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

Asked for by the maintainer: check for a new release and update the program before
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
  **When I close** (named **Install on next launch** from 0.5.12: what
  the player sees is the next start installing it; the files are still
  put in place as the launcher closes).
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

Asked for by the maintainer: Add to Steam starts the game without the launcher, so
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

Asked for by the maintainer: the project's Ko-fi page in the game's menu, and once
when the game starts, as in pcbview (a one-time notice at its first start, and
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

The maintainer ran the 0.5.0 release's launcher under plain Wine 11.21 (not Proton)
and reported three things: no way to change where the game goes, the
launcher taking their ROM folder without asking, and a build stuck at
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
`launcher.log`. The maintainer's decision: Proton only. The launcher keeps SDL's
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

**Add to Steam only from outside Steam** (the maintainer's decision, the same
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

The maintainer's report: under CachyOS (KDE, a 4K screen scaled 175%) the launcher
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

The maintainer asked for a Steam entry that plays the game without the launcher.
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

The maintainer played a release under Steam (Proton) with a DualSense: every
button moved player 1 and player 2 at once.

**Why (from that run's `vf4ft.log` and Steam's environment):** with Steam Input
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

**On the maintainer's machine (2026-11-13),** a 0.5.2-dev test copy added to Steam
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

The maintainer asked for mouse clicks in the game's menu (the launcher had them;
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

The maintainer asked what the red block behind the first letter of the boot's
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

The maintainer asked for it to be easy: open the launcher, have an option to create
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

The maintainer asked for a warning when Play is pressed on Linux outside Proton and
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
  it held 60 frames a second through the attract sequence. The maintainer's own run
  (12:28) found their DualSense through Wine's HID. In a headless KWin with
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

The maintainer asked whether networking could be turned off so the start-up's
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
0x162A in the record. The maintainer's backup: both checks hold, both copies equal,
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
audio; copies of the maintainer's backup):
- The maintainer's backup, default settings: "network: off", the log screen at
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

The maintainer's report: the Steam entry that Add to Steam makes has no artwork and
no description. The choice for the pictures: made from the player's own build, on
the player's machine, so no game pictures are in the repo or the release.

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

Not tried: a real Steam showing the pictures (the maintainer's to see: Add to Steam
with Steam closed, then start Steam), Windows itself, and the Deck.

## 37. Card profiles, the launcher's Cards tab and the card editor (2026-11-13)

The maintainer's request: a "character profile" per player, as the cabinet's card
was; a default profile to start from, new ones made from it, and each
assigned to a player. The choices: the editor in the launcher first, a
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
Character row is what the requested "character profile" is.

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

*Versus record (2026-11-13):* the maintainer won a fight against the computer and
their card still had no wins. The game counts only versus matches there:
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
item numbers each character has (the editor goes by number; the maintainer's
earlier choice).

## 38. The sky above a victory pose: the guard band (2026-11-13)

The maintainer saw the top of the screen black at the end of a fight, while the
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

The maintainer asked for wide screen options with no stretch, for the Steam Deck
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

**The library's placeholder (2026-11-15).** Every wide picture had one
black pixel just left of the 4:3 picture's top-left corner, on both
renderers and every GPU (0.5.8 and before). The Naomi 2 library puts a
placeholder in each of its lists every frame (`dummy_tri_link_on_lists`):
sprites from (-1, -1) to (-0.1, 1) at depth 0.1, and in the
modifier-volume lists triangles there, left of the screen so that on the
chip they cover no pixel. `keep_left` kept them; the translucent list's
copy blends with both factors zero, so the pixel was black. A temporary log
in `scene_draw` (removed) found them the only triangles at that spot.
`chip_vertex` now drops them, matched by the library's own words corner for
corner and by their header (`list_placeholder`), so nothing else the game
draws can match. At 4:3 they were never drawn. Checked (`--shot`, the
headless KWin, the attract sequence at seven moments, before and after the
change): 4:3 is pixel for pixel the same on both renderers; at 16:9 the one
pixel, (106, 0), is the only one changed, from black to the scene's colour,
on both.

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

The maintainer's report: with the Resolution row at 4x and a 16:9 picture, the
window stayed 1,281 x 720 on a 4K screen. The row was the GPU
renderer's size (3,416 x 1,920 there, scaled down to the window), and the
window had a fixed size of its own (section 39). The maintainer asked for both: an
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

The maintainer asked how to change the game's region, and for a region choice in
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

The maintainer's request: no card given to a player by default ("When a card is
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
- the Windows game: the maintainer played the test copy (built from these
  sources) on 2026-11-14 with no problems.

Not checked: a Steam Deck's keyboard, an edit waiting when the machine
stops.

## 43. More window sizes (2026-11-14)

The maintainer asked why Window size offered so few sizes, with lists of the usual
16:9, 4:3 and 21:9 screens' resolutions. The list (section 40) was the GPU
renderer's four heights and the screens' 720, 1080 and 2160; nothing else
limited it. Two things were missing: the heights of the other usual
screens, and the ultrawide screens' own widths. A 21:9 window was always
3440 x 1440's shape (43:18), but 2560 x 1080 and 5120 x 2160 are 64:27 and
3840 x 1600 is 12:5, so a "1080" 21:9 window was 2,580 wide (too wide for
the screen it is named after, so made smaller to fit).

**The heights** (`settings_window_heights`, 16 of them): 480, 576, 600,
720, 768, 800, 900, 960, 1050, 1080, 1200, 1440, 1536, 1600, 1920 and
2160. At the picture's shape they give every size of those lists: 16:9
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

The maintainer asked for the game to know the Steam Deck and the Deck OLED and to
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
the same number of refreshes (only while the timer and the screen keep
the same time, which they do not for long: section 58, where the pace
follows such a screen instead); at 90 Hz (the OLED's top rate) pictures are
shown for one or two refreshes in turn, at 144 Hz for two or three, and
the motion judders. The game cannot set the screen's rate: on a Deck it
is Steam's (the Quick Access button, Performance, Use per-game profile,
Refresh rate), elsewhere the system's. So it says so, once:

- `settings_refresh_uneven(hz)`: the rate is known and not within 1 Hz of
  a multiple of 60 (59.94 and 120 are even; 90, 75, 144 and 165 are not).
- Two seconds into the picture (after the Ko-fi notice, section 26, when
  that shows), main.c reads the window's screen's rate
  (`SDL_GetCurrentDisplayMode`, logged as `screen: 144.00 Hz`; since
  section 61 that report is held against the rate the presents show, as
  it can be an old one, and the notice waits for that rate to settle). When it
  is uneven, or on a Deck OLED whatever it reports (the per-game rate is
  Steam's, and what gamescope reports to the game is not known to follow
  it), the menu opens as a notice (`MENU_NOTICE_REFRESH`): "Set the
  screen to 60 Hz", the steps, and OK. (Off a Deck the advice is "a
  multiple of 60 Hz" since section 61, and a rate the game counted itself
  is said as a guess.)
- `user/refresh-notice` says what it was shown for, `Steam Deck OLED` or
  the rate (`144 Hz`), on its first line: it shows again only for
  another screen. Test runs (`--frames`, `--keys` and so on) show it only
  with `--refresh-notice N` and write nothing; `--deck-product FILE` and
  `--screen-hz HZ` stand in for the firmware's name and the rate
  (`--system-hz HZ` for the system's report alone: section 61).
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

The maintainer asked about two things: a VF.NET server like Flycast's, and two
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
   or a small relay the maintainer runs; rollback (the machine saved each frame,
   frames run again when a late input differs), which needs a fast
   in-memory snapshot (`snapshot_save_mem` goes through a file). The
   machine takes about 3 ms a frame on a desktop, so it fits there; the
   Steam Deck (an estimated 10 to 12 ms) stays on input delay.
3. Public matchmaking: a lobby, a queue by ping, a relay so players never
   see each other's addresses. A decision for later.

**Cards live with their player** (the maintainer's rule). A card's file stays on
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
chip runs a fixed budget a sample; since section 77 the game window's
clock starts at the PC's date and time, and a two-PC game's at the host's
on both PCs), and compiled arithmetic is IEEE on
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

**GPU: the choice.** (Section 53 changes this: Auto is now the first GPU
in the list that works, a chosen GPU that cannot be had gives way to the
next in the list, and the launcher's window follows the row too.) Auto
(the default) is SDL's choice, as before. The
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
next start (the menu's dot; at once since section 53), and the menu's note says which GPU draws
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

## 52. The launcher's loop waits for work; Vulkan first under Wine (2026-11-15)

Reported against 0.5.7: under Proton, after the launcher's window was
dragged bigger, the page stayed at its old size in the top-left corner,
black around it, and clicks landed in the wrong places. Plain Wine did the
same (section 27 saw it there and left it: plain Wine is not supported).

**Where the fault is: not the launcher.** On the machine it was reported
on (an AMD Radeon 8060S with RADV, and an NVIDIA card in an eGPU):

- The NVIDIA card alone (`VK_DRIVER_FILES` naming only its driver): the
  launcher resizes correctly. The Radeon alone: it breaks, both with
  Proton's Direct3D 11 (DXVK) and with Wine's own Direct3D 12.
- A 100-line SDL program with nothing of the launcher in it, an 8-pixel
  red border drawn on the picture's edges, breaks the same way on the
  Radeon: the border stays where the old size ended, while SDL reports,
  every frame, an output size equal to the window's, and no error.
- The same program with SDL's Vulkan renderer (`SDL_RENDER_DRIVER=vulkan`
  under Wine) on the same Radeon follows the window, and so does native
  `vkcube` there.
- SDL's Direct3D renderers present with `DXGI_SCALING_NONE`, so a picture
  left at the old size means the buffers that reach the screen never grew.

So the fault is in the Direct3D-over-Vulkan layers on RADV: Wine's own
Direct3D 12 swapchain and DXVK's. The real fix is theirs, and the test
program is the case to report with (not reported yet).

**The workaround: Vulkan first under Wine.** A launcher that finds
`wine_get_version` in ntdll (plain Wine and Proton both have it) asks SDL
for its renderers in the order
`vulkan,direct3d11,direct3d12,direct3d,opengl,opengles2,gpu,software`:
SDL's own Windows order with Vulkan moved to the front. SDL tries them in
turn, so a system without a Vulkan driver gets the next one that starts.
`SDL_RENDER_DRIVER`, when it is set, still decides (an environment
variable outranks `SDL_SetHint`). `launcher.log` says which, as in
`window: SDL 3.4.20, renderer vulkan (Vulkan asked for first under Wine),
video windows`. Windows itself keeps SDL's order. This reverses section
27's choice to keep SDL's order, which was made when only plain Wine's
Direct3D 12 was seen to fail; Proton's DXVK now fails the same way on
the Radeon. The game is not touched: its SDL_GPU already takes Vulkan
under Proton (section 20).

**The loop no longer counts on VSync.** The launcher's loop used to poll
its events, draw and present with VSync on, so the present's wait for
the screen was its only pause. Two faults came with that, both ours:

- A renderer whose VSync does not wait made it spin. SDL's Direct3D 12
  makes its swapchain with a frame-latency waitable object and never
  waits on it: the test program drew about 17,700 frames a second under
  Wine's Direct3D 12, and section 27 saw the launcher use 60% of a core.
- Where VSync does wait, the page was drawn at the screen's rate (60,
  144, or whatever VRR gives) when nothing on it changed.

Now the loop sleeps in `SDL_WaitEventTimeout` until something needs it,
and draws once each time it wakes:

- an event: input, the window, a pad, or a folder picker's answer (the
  picker's thread pushes an event of the launcher's own);
- a build or an update check running: its output read every 50 ms, as
  before;
- the game running: watched every 100 ms. The window is hidden then, and
  SDL's wait with no window shown wakes every millisecond, so the loop
  uses `SDL_Delay` and a poll instead;
- something on the page that moves by itself: a bar of unknown length
  asks for the screen's next frame, a text cursor for its next blink
  (530 ms). A hidden or minimized window asks for neither;
- otherwise once a second.

Two frames start at least 9/10 of the screen's frame apart (`frame_gap`),
so a mouse moving or a held key draws no faster than the screen shows,
whatever the renderer's VSync does. VSync stays on, only to keep the
picture from tearing. With a pad open, SDL's own wait wakes each
millisecond to read it (`EVENT_POLL_INTERVAL_NS` in SDL_events.c) and
goes back to sleep without returning: that is SDL reading the pad, with no
drawing.

(Section 53: this loop could draw once more as its Wayland compositor went
away, and that frame never finished, at full CPU. It now draws no more
then, and a thread of its own ends a launcher stuck in such a frame.)

**Checked** in a headless KWin (a 1920 x 1080 virtual screen at 60 Hz;
nothing reaches the real desktop). CPU is the launcher process's own
(its /proc times over 10 s, as a share of one core). Frames were counted
by a test-only `SDL_RenderPresent` wrapper (LD_PRELOAD) for the Linux
launcher, and for the Windows one by a test-only copy of the source that
logs its wakes and frames every 5 s; the same wrappers turned VSync off,
as a renderer whose VSync does not wait.

| Launcher, idle on the Play page | Renderer | Frames a second | CPU |
|---|---|---|---|
| Linux, before, VSync on (X11) | opengl | 60 | 3.6 to 5.5% |
| Linux, before, VSync off | opengl | about 5,300 | 99% |
| Linux, now, VSync on or off (X11) | opengl | 1 | 0.0 to 0.2% |
| Linux, before (Wayland) | opengl | | 4.0 to 4.4% |
| Linux, now (Wayland) | opengl | | 0.1 to 0.2% |
| Windows 0.5.7 under plain Wine | direct3d | | 4.8 to 7.8% |
| Windows, now, plain Wine, Vulkan on lavapipe | vulkan | 1 | 0.6 to 1.0% |
| the same, no Vulkan driver at all | direct3d | | 0.1 to 0.3% |
| the same, `SDL_RENDER_DRIVER=direct3d` | direct3d | | 0.1 to 0.2% |

(In that session plain Wine's Direct3D 11 does not start and its Direct3D
12 does not start on lavapipe, so SDL took Direct3D 9, `direct3d`, both for
0.5.7 and as the fallback.) With Vulkan on lavapipe the rest of the CPU is
lavapipe's and Wine's threads: the loop itself woke 5 times in each 5 s.

- **The pointer moving** 500 times a second over the window (Wine's
  `SendInput`, 10 s): 60 frames a second with VSync on, and 66.8 with it
  off, which is `frame_gap`'s limit at 60 Hz (the Linux runs could not
  move the pointer: XTEST's moves do not reach the nested session's
  windows). Back to 1 a second when it stops.
- **Resizes** (three, by a KWin script): a few frames for the size changes,
  then 1 a second. Clicks on Check for updates before and after them land
  (each logs a new check), and the pictures at 1:1 fill the new size.
- **Renderer choice:** under plain Wine the log says `renderer vulkan
  (Vulkan asked for first under Wine)`; with no Vulkan driver SDL went on
  to `direct3d`, and the launcher worked the same; with
  `SDL_RENDER_DRIVER=direct3d` the log says `renderer direct3d
  (SDL_RENDER_DRIVER)`. The Linux launcher's order is SDL's own (opengl).
- **Builds** under plain Wine with Vulkan on lavapipe, from an empty
  install folder: `--do build --exit` built the game in 75 s (the first
  download of llvm-mingw included) and ended with 0; `--cancel-after 20`
  pressed Cancel at 20.4 s during translation, the builder stopped in the
  same second, and the launcher ended with 1. The build page drew its step,
  bar, elapsed time and log as before. While a build runs the launcher
  draws at the screen's rate when its bar's length is unknown, and every
  50 ms otherwise (its output is read then); on lavapipe that drawing is
  the 15 to 35% of a core the launcher used, which a GPU does instead.

**Not covered.** Proton itself on the Radeon (the reported case: to be
tried on that machine), VRR, a folder picker's answer (no picker in the
headless runs; without the wake the loop still reads it within a second),
a text cursor's blink, and a pad connected.

## 53. Renderer and GPU: an order to fall back on, changed at once (2026-11-15)

Asked for: by default the first GPU that works, then Software; a GPU
chosen is tried first, then the first that works, then Software; and the
renderer changed on the fly, in the game and in the launcher.

**One setting for both programs.** The Picture tab's Renderer and GPU rows
(`renderer` and `gpu` in settings.txt) now decide the launcher's window as
well as the game's. Before, the GPU row was the game's only, and the
launcher used SDL's own renderer order.

**The order.** The GPU chosen; then the other GPUs in the GPU row's order
(devices.c gpus_list: Vulkan's order); then Software. Auto starts at the
top of the list. A GPU works when it opens, takes the window, and its
renderer starts. A chosen GPU that is not there is logged and kept in
settings.txt, as before (section 51), and is tried first again once it is
back.

**What this changes about Auto.** Before, the game's Auto was SDL_GPU's
own pick, which ranks a separate card first (section 51), and the Linux
launcher took SDL's own order (OpenGL). Now Auto is the first in Vulkan's
list, and Mesa's device-select layer puts the GPU the desktop is drawn on
first:

- On this PC's own desktop (KWin on the Radeon), `vulkaninfo --summary`
  lists the Radeon 8060S, then the RTX 5070 Ti. Auto there moves from the
  5070 Ti to the Radeon, the GPU the picture is shown on, so no frame is
  copied between GPUs. Choosing the 5070 Ti in the GPU row puts it back.
- In the headless KWin of the tests, which renders on the NVIDIA (section
  51), the NVIDIA comes first.

**The game** (main.c video_open, video_close, video_switch):

- video_open builds the order: the GPU chosen, the other listed GPUs,
  then SDL_GPU's own pick if it is not one of them. It tries each in
  turn: open it (section 51's two ways to open a GPU by name), claim the
  window, start the GPU renderer (gpu_render_init). The first that does
  all three draws.
- A device that claims the window but cannot start the GPU renderer is
  kept, to show the software picture if no GPU can draw.
- Software (asked for, or no GPU works): the software picture, shown
  through the first device that claims the window, else SDL_GPU's own
  pick. It still needs one: with no Vulkan device at all (and no Direct3D
  12 on Windows) the game cannot show a window, as before. lavapipe
  counts as one.
- Log lines: "renderer: GPU, 1280 x 960, on NAME (the GPU chosen)", "(the
  GPU chosen could not be used)", "(the first that works: the GPU chosen
  is not here)", "(Auto: the first that works)"; or "renderer: software,
  640 x 480, shown through NAME".

**Changed as the game runs.** The menu's Renderer, GPU and Internal
resolution apply at once (no dot now). The menu pauses the game (section
22), so the change falls between the machine's frames:

- The menu's change (MENU_LIVE) asks the window's loop to switch
  (video_redo). The loop closes the renderer and its device (video_close)
  and opens them again by the order (video_open). Internal resolution
  alone, on the same GPU, only restarts the GPU renderer on its device.
  (Since section 60 the device is not closed there: it gives the window
  back and is held, and a GPU's device is opened only once in a run.)
- The machine's thread either hands its passes to the GPU renderer or
  keeps them for the software picture (game_machine_gpu), and may only
  change that on its own thread. So the window's loop publishes the
  choice (`gpu_path`), and the machine's thread takes it up at its next
  vertical blank, in on_frame. It logs "vertical blank N: the machine
  draws for the software picture from now on" (or "for the GPU
  renderer").
- gpu_render's CPU side (its lock, packet lists and texture upload queue)
  outlives the device, so the machine may submit at any time. After a new
  init, the next submit uploads the whole texture memory in use (`up_all`):
  the new device has none of it.
- The menu's overlay keeps its software side (the backdrop and the UI)
  and makes its GPU textures again on the new device (ovl_gpu_free,
  ovl_gpu_make).
- The notes say what draws now: "Drawing on NAME now.", "Auto: drawing on
  NAME.", "NAME could not be used: drawing on OTHER.", or, for the GPU
  renderer with no GPU, "The GPU renderer could not start on any GPU: the
  game is drawn with Software for now."

**The launcher** (launcher.c renderer_make, renderer_follow):

- SDL's renderer API cannot be given a GPU by name either. But its Vulkan
  renderer takes a Vulkan instance, surface and physical device made by
  the program (SDL_PROP_RENDERER_CREATE_VULKAN_*, with the graphics and
  present queue families).
- devices.c vk_window_open makes them for the launcher's window:
  - an instance with SDL's extensions, plus
    VK_KHR_get_physical_device_properties2 when it is there (SDL's renderer
    looks for it);
  - the window's surface;
  - the GPUs that can show the window (a graphics queue, a queue that
    presents to the surface, VK_KHR_swapchain), by the GPU row's names.
    CPU devices are left out, as in the row.
- renderer_make tries SDL's Vulkan renderer on the GPU chosen, then on
  each of the others. Then it takes SDL's own order (Vulkan first under
  Wine, section 52), which ends with Software. With Software asked for,
  it uses SDL's software renderer.
- The renderer is made again at once, on the same window, with the UI's
  textures (ui_quit, ui_init):
  - when Renderer or GPU changes in Settings;
  - when the game ends (its menu may have changed them);
  - when another install folder is chosen (it has its own settings).
- SDL_RENDER_DRIVER, if set, still decides, and --shot pictures use
  SDL's own choice.
- Log lines: "renderer: vulkan on NAME (the GPU chosen)", "(the GPU
  chosen could not draw)", "(the first that works)", "renderer: software
  (the Renderer setting)", and "renderer: Renderer Software, GPU Auto
  (Settings): made again".
- The Settings note says what the launcher draws on now, as the game's
  menu does.
- The "window: SDL ..." line no longer says why. The "renderer:" line
  says it, including "SDL's own order, Vulkan first under Wine".

**Tested** in headless KWin (one 1920 x 1080 virtual output, Xwayland on;
nothing on the real desktop). The game ran natively; the launcher ran
natively and as the Windows build under Wine 11 (a scratch prefix, with
small test installs: a build.json and user/settings.txt). Pictures were
checked at 1:1.

| Case | Result |
|---|---|
| Game: Auto (Wayland) | the first in the list (here the NVIDIA) |
| Game: the Radeon chosen (Wayland) | drawn on it ("the GPU chosen") |
| Game: a GPU that is not here | the first that works, logged, the setting kept |
| Game: the Radeon chosen on X11 (the X server on the NVIDIA) | its swapchain fails, so the NVIDIA draws ("could not be used"); the menu's note says so |
| Game: Software | the software picture, shown through the first GPU |
| Game menu as it runs: GPU, Software, GPU; Auto, NVIDIA, Radeon; 1x, 2x, 3x | each at once; the picture drawn after each; the machine's change logged; settings saved |
| Launcher (Wayland, and Windows under Wine): Auto, the Radeon, a GPU not here, Software | as the game. Under Wine the Radeon cannot make a swapchain (the X server is on the NVIDIA), so the NVIDIA draws ("could not draw") |
| Launcher: another install folder (`--pick-game`) | made again with that folder's Renderer |
| Launcher Settings as it runs (Windows under Wine) | Software, GPU, the NVIDIA, the Radeon (fell back), Auto: each made again, and the page drawn right after |
| Launcher under Wine, only lavapipe / no Vulkan | SDL's own order: Vulkan on lavapipe / Direct3D 9 |

The descriptions were shortened after the first test: the GPU row's text
and a fallback note with two long GPU names ran past the bottom of the
launcher's description card.

**Not tested:**

- A GPU that opens and claims the window but whose GPU renderer cannot
  start: there is no way to force that without a test hook. The code keeps
  that device for the software picture.
- A GPU lost while the game runs (an external GPU unplugged): not handled,
  as before.
- The launcher's Settings switch natively. Neither XTEST's keys nor its
  pointer reach native windows in the headless session (section 52). The
  same renderer_follow ran natively for `--pick-game`.
- The launcher following the game's menu when the game ends (the same
  function).
- Proton itself, Windows, the Steam Deck, and this PC's own desktop.

### The compositor going away (found on the way: section 52's loop)

Section 52's launcher spun at full CPU, and SIGTERM did not stop it, when
its Wayland compositor went away under it: with OpenGL, Vulkan and the
software renderer alike. It was seen in the headless runs, where KWin ends
with the test. The 0.5.7 launcher closes then ("Wayland display connection
closed by server").

**What happens.** Found with a debug build of SDL 3.4.20 (from
`toolchain/sdl3-src`, built apart and loaded with `LD_LIBRARY_PATH`) under
gdb, and `WAYLAND_DEBUG`:

- KWin killed, as a crash: SDL's event pump finds the connection gone,
  logs "Wayland display connection closed by server (fatal)" and sends its
  quit. The loop's `running` went to 0, but the loop still drew at the end
  of that turn, and that frame never finished.
- KWin ending cleanly: it takes its screen away first
  (`wl_registry.global_remove`, `wl_surface.leave`), which wakes the
  launcher, and it draws. KWin closes the connection during that frame:
  the present's own flush fails (`ECONNRESET`; `wl_display_get_error` 104)
  and SDL's `display_disconnected` stays false. SDL's pump would find it,
  but the pump never runs again.
- Where the frame never finishes: with OpenGL, and with the software
  renderer (on Wayland it presents through OpenGL), in SDL's
  `Wayland_GLES_SwapWindow`, which waits for the frame callback
  (`SDL_waylandopengles.c` line 156). Once the connection has failed,
  `wl_display_prepare_read_queue` keeps saying events are queued,
  `wl_display_dispatch_queue_pending` will not dispatch them, and the loop
  goes back to its top without looking at its own 50 ms limit. With Vulkan
  on the NVIDIA card the frame did not finish either, mostly in the kernel;
  under gdb that case did not happen (it depends on timing), so it was not
  traced.
- SIGTERM: SDL turns it into a quit event, which the stuck main thread
  never reads.

So the cause is in SDL (and for Vulkan, probably in NVIDIA's driver), not
here. What the launcher does about it:

- **Before each frame** (`display_gone`) the loop asks whether the
  compositor has gone: libwayland's `wl_display_get_error`, or the
  connection's socket hung up. If it has, or the loop is closing (SDL's
  quit), no frame is drawn and the launcher closes as usual: "window: the
  compositor has closed its connection: the launcher closes".
- **A frame already being drawn** when the compositor closes the
  connection: the main thread cannot get out of it. So a thread of its
  own (`watch_run`, in `runtime/host/compositor.c` since section 54)
  sleeps on the connection's socket: `poll` with no events
  asked for wakes for a hang-up or an error only, and it takes none of
  libwayland's locks (a stuck thread may hold one). Once the socket has
  hung up, a frame still being drawn 3 seconds on ends the launcher there
  (`_exit`): "window: the compositor has closed its connection and the
  frame being drawn has not finished in 3 s: the launcher ends". That
  skips the launcher's closing steps: a build stops as its input ends, and
  a download is taken up again by the next start's check.
- Wayland only. Windows is not changed; X11 was not looked at.

**Tested** in the same headless KWin: five launchers left running, their
windows over each other, as KWin ends, 5 times cleanly and once killed.
Nothing was left running after any of them.

| Launcher | KWin ends cleanly | KWin killed |
|---|---|---|
| 0.5.7 | closes (SDL's quit), 5 of 5 | closes |
| Section 52's loop (main) | spins | spins |
| With the check before each frame only | still spins: Vulkan on the NVIDIA 3 of 3, software 2 of 3, OpenGL 2 of 3 | closes |
| With the watch: Vulkan on the NVIDIA | the watch ends it, 5 of 5 | closes (SDL's quit) |
| With the watch: Vulkan on the Radeon | closes (SDL's quit), 5 of 5 | closes |
| With the watch: software | the watch 4 of 5, SDL's quit 1 | closes |
| With the watch: OpenGL | the watch 2 of 5, SDL's quit 3 | closes |

A scratch build that logged each frame's start showed that the frame that
never finishes starts as KWin takes its screen away, and that the watch
ends the launcher 3.0 seconds after that start. With KWin kept for 12
seconds first, no present took 200 ms or more in any of the four: frames
do not stall while the compositor is there, even in covered windows.

**The game was not fixed here.** It closes when KWin ends cleanly (3 of 3). When
KWin is killed, it stays, asleep (no CPU): its main thread waits in
NVIDIA's Vulkan driver (`drmSyncobjTimelineWait`, under SDL) for a buffer
the compositor never gives back. The game writes its backup every 10
seconds and as it ends, so ending it the launcher's way needs more care:
section 54.

## 54. The game and the compositor going away (2026-11-15)

Section 53 left the Linux game running, asleep, when its Wayland
compositor was killed. Asked for: find why, and have the game end, its
backup saved, within a few seconds of a compositor crash, while a
compositor ending cleanly still closes it as before.

**Where it waits.** Found in the headless KWin (KWin killed with SIGKILL,
as a crash) with a debug build of SDL 3.4.20 (from `toolchain/sdl3-src`,
loaded with `LD_LIBRARY_PATH`) under gdb. The main thread is in
`SDL_WaitAndAcquireGPUSwapchainTexture` → SDL's
`VULKAN_INTERNAL_AcquireSwapchainTexture` → `vkAcquireNextImageKHR` with a
timeout of `SDL_MAX_UINT64` (`SDL_gpu_vulkan.c` line 10205) → the driver →
`drmSyncobjTimelineWait`. It is the acquire, not a fence wait. It is the
same with NVIDIA's driver (620.70.06) and with RADV on the Radeon.

**Why it never returns.** The swapchain uses explicit sync: with
`WAYLAND_DEBUG=client` the game sends
`wp_linux_drm_syncobj_surface_v1.set_acquire_point` and `set_release_point`
with every commit. A buffer is free again only once the compositor signals
its release point. A killed compositor never does, the kernel's wait has
no end, and neither driver's Wayland code watches the connection while it
waits. The game draws at 60 frames a second, so its main thread is almost
always in that wait when the compositor goes. SDL's event pump never runs
again: no "connection closed by server", and the quit that SIGTERM makes is
never read. (SDL's Vulkan renderer, the launcher's, acquires with no time
limit as well, `SDL_render_vulkan.c`; the launcher draws only when
something moves (section 52), so it is seldom in an acquire.)

**The machine is not stuck.** Its thread runs on, paced, and writes
`backup.bin` every 10 seconds after the compositor has gone: the file's
time moved after the kill in every run. Only the window's thread waits.

**Not a regression.** Main's game before section 53 (cb3aa5d, built apart)
does the same. Before this section's change, sound off, the game left
running 8 seconds after KWin ended:

| Game | KWin killed | KWin ends cleanly |
|---|---|---|
| Section 53, GPU renderer on the NVIDIA | stays, 2 of 2 | closes, 9 of 9; 5 of them crash (below) |
| Section 53, Software (shown through SDL_GPU on the NVIDIA) | stays, 1 of 1 | closes, 3 of 3 |
| Section 53, GPU renderer on the Radeon | stays, 2 of 2 | **stays, 6 of 6** (under gdb it closed, 1 of 1) |
| cb3aa5d on the NVIDIA | stays, 2 of 2 | closes, 2 of 2; both crash |
| cb3aa5d on the Radeon | stays, 1 of 1 | stays, 1 of 1 |

On the Radeon even KWin ending cleanly leaves a buffer it never gives
back: the main thread's kernel wait (`/proc/PID/task/PID/wchan`) is
`drm_syncobj_timeline_wait_ioctl`, 2 of 2, without gdb.

**A second fault: NVIDIA's driver crashes closing the window.** When KWin
ends cleanly on the NVIDIA the game closes, but in 7 of 11 runs (both
builds) it ended with SIGSEGV (exit code 139) and no "clean exit" line.
The core dumps (`coredumpctl`; two of them with the debug SDL) all show
`video_close` → `SDL_ReleaseWindowFromGPUDevice` → SDL's
`VULKAN_INTERNAL_DestroySwapchain` → `vkDestroySwapchainKHR`
(`SDL_gpu_vulkan.c` line 3360) → NVIDIA's driver → a jump to an address in
the heap: a stale pointer in the driver once the connection has gone. It
happens after the machine has stopped and saved, so nothing was lost, but
the exit code is wrong and a 40 MB core dump is kept each time. The cause
is in NVIDIA's driver, which this project cannot change.

SDL 3.4.20's Wayland reconnect (`Wayland_VideoReconnect`) is compiled out
(`#if 0`, waiting on KWin's restart work), so a game cannot carry on under
a compositor started again anyway: ending it, saved, is the most it can do.

**What the game does now.**

- **The watch, shared with the launcher.** Section 53's watch moved to
  `runtime/host/compositor.c` (both programs build it; `build.py` too), its
  behaviour unchanged: a thread sleeps on the Wayland connection's socket
  (`poll` with no events asked for), and once the compositor has closed it,
  a stretch the main thread marked (`compositor_busy`) still going 3
  seconds on is ended by the watch's thread. The program's own function
  (the launcher's `frame_stuck`, the game's `window_stuck`) says why and
  does what must come first; then `_exit` with the code it returns. A main
  thread that comes out of its stretch after the watch has started this
  waits there for the end (the stamp is taken with a compare-and-swap), so
  the two threads never both close the game.
- **The game's stretches:** a frame, from the swapchain's acquire to the end
  of that turn's GPU work (submit, shots, read-backs); a renderer change
  from the menu (`video_switch`); and closing the window at the end.
- **`window_stuck`** (on the watch's thread) does the main thread's exit
  steps for the machine (`machine_stop_ask`, now shared with the normal
  exit: the shot and menu waits let go, `window_quit`, a two-PC game
  cancelled, `game_machine_stop`), then waits up to 5 seconds for the
  machine thread's own end: as on any quit it writes `backup.bin` and the
  cards (`machine_main`). Then `session_end` (the other PC is told),
  `gu_quit` (an update asked for in the menu still goes to the launcher),
  and exit code 1. A machine that has not stopped in 5 seconds is logged;
  the backup it wrote last stays. Nothing of the window is closed.
- **Before each frame** (`display_gone`): libwayland's
  `wl_display_get_error`, or the socket hung up. If the compositor has
  gone, no frame is drawn and the loop ends the usual way, the machine
  stopping and saving.
- **A workaround for NVIDIA's crash:** at the end, once the machine has
  stopped and saved, a compositor that has gone means the window is not
  closed (`gone_close`): `gu_quit`, a line in the log, and `_Exit` with the
  usual code, so NVIDIA's swapchain destruction never runs. Nothing is
  lost by it: the process is ending, and the kernel frees what it held.
  With the compositor there the game closes exactly as before.

The log says which way it went: "window: the compositor has closed its
connection and the frame being drawn has not finished in 3 s: the machine
stops, and the game ends once it has saved", or "window: the compositor's
connection has failed (...): the game closes"; then "machine stopped" and
"the compositor has gone: the game ends without closing its window (exit
code N)".

**Tested** in the headless KWin, one game at a time, sound off, KWin
ending 12 seconds after the game started (in the attract sequence). Times
are from the kill, or from the end of KWin's session, to the game's end.
Nothing was left running after any run, `backup.bin` was written after
KWin ended in every run, and no core dump was made:

| Game | KWin killed | KWin ends cleanly |
|---|---|---|
| GPU renderer on the NVIDIA | the watch, 3 of 3, 3.13 to 3.15 s after the kill; exit code 1 | the check before a frame, 5 of 5, 0.26 to 0.29 s; exit code 0 (no crash) |
| GPU renderer on the Radeon | the watch, 3 of 3, 3.06 s; exit code 1 | the watch 2 of 3 (3.18 and 3.19 s; exit code 1), the check 1 of 3 (0.18 s; exit code 0) |
| Software (on the NVIDIA) | the watch, 1 of 1, 3.13 s | the check, 1 of 1, 0.29 s |

Also tested:

- The menu open as KWin is killed (the machine waits for it): the watch
  ended the game 3.17 s on, the machine let go of the menu's wait and
  stopped. Its backup had not changed since the save 2 seconds before,
  so it was not written again (`game_machine_backup_get` gives only a
  changed one).
- The game ending by itself (`--frames`) with KWin there: as before,
  "clean exit", exit code 0.
- The launcher on `compositor.c`: section 53's test again, five
  launchers and the game, KWin ending cleanly 3 times and killed once.
  Nothing was left running; each launcher closed as in section 53's
  table (the watch, or SDL's quit), and the game with them by the check
  before a frame (exit code 0) or, KWin killed, by the watch (exit code
  1).
- The Windows build (llvm-mingw) of both programs compiles
  (`compositor.c` is empty there).

Not covered: X11 (and the game under XWayland) was not looked at, and
Windows is not changed; Proton's game is the Windows build. A two-PC game was not tried with the compositor
going away.

## 55. Rollback's first part: going back, and the one-PC sync test (2026-11-16)

The two-PC game is lockstep with an input delay (sections 46 to 50).
Rollback would let each PC run ahead on a guess of the other player's
controls and, when the real ones arrive and differ, put the machine back
to the frame they belong to and run the frames since again. This section
builds the part every rollback needs, and nothing of the network: the
machine kept at the end of each of the last few frames, the way back to
any of them, and a test on one PC that shows the machine run again from
there is the machine it was.

**What a frame costs to keep** (measured first, on the desktop, a scripted
two-player fight): the machine's own work is 2.28 ms a frame. The memory a
frame writes is 0.71 MB at 4 KiB granularity, 2.9 MB at 64 KiB. Copying
what changed takes about 0.06 ms; the models' state (everything a snapshot
holds outside the memory areas: the processor, the hardware registers, the
event table and every module's `snapshot_register` blob) is about 2.3 MB
and takes about 0.09 ms to copy whole.

**Which pages a frame writes** (`runtime/machine/mem_track.h`, `mem.c`).
Translated code and the interpreter store through `mem_wpage`, a NULL entry
sending the store to the slow path (`rt_io_write`, `mem_code_write`,
`rt_sq_write`). While rollback is on, a 64 KiB page of a memory area not
yet written since the frame began is *armed*: NULL in `mem_wpage` at every
address it appears at (each of the six mirrors, and the aliases), so its
first store takes the slow path, which calls `mem_track`. That shows the
page to rollback, which copies its 64 KiB away, and puts it back in
`mem_wpage`; later stores to it that frame take the fast path again. The
fast path is unchanged, so a frame pays only for its first store into each
page (a slow call and a 64 KiB copy). The host's own stores (`g_w32` and the
rest, through `HOST_STORE`; the ELAN's plain transfers, the controller's
replies, the BIOS stand-in) call `mem_track` too, and device code that
writes guest memory through a host pointer calls `mem_track_host` first:
the ARM7's stores into sound RAM, the sound DSP's ring, the sound driver's
block moves, a pass the renderer stores into a texture, the program's load
and a snapshot's load. The pages that always take the slow path (low RAM,
the first 64 KiB of sound RAM) are shown by `ram_slow_write`. 4 KiB pages
would copy a quarter of the bytes but need a table 16 times larger;
`mprotect` and a fault handler were not used, since every store already
goes through code this project generates or owns.

**The ring** (`runtime/machine/rollback.c`). At each frame's end
(`rb_frame`, from the end of `host_frame` once the game has booted, its
controls in) the models are copied whole (`snapshot_dev_save`, machine.c)
into the newest record, and the pages written since are armed again. A
record holds that frame's models and the pages the next frame wrote, as
they were before. `rb_restore(F)` puts back the pages of every record from
the newest to F's, newest first (what is left of each page is its oldest
copy, F's), then F's models, and arms every page. The 64 KiB copies come
from a pool and go back to it, so after the first few frames nothing is
allocated.

**Going on from there** (`rt_native_rewind`, rt_native.c). The run starts
again from `rt_native_run`'s frame, as after a snapshot's load from the
frame hook (`rt_native_load`), with the processor where it was at that
frame's end: inside the vertical blank's event, before the interrupt is
taken, which `rt_event` then does as the first run did. It skips
`code_refit`: every byte of the program that differs from its own was
marked when it was stored or loaded, and marks are never taken back, so the
marks still cover the code as it was at F. As after any load, the code
around the routine the run was in goes on in the interpreter
(`rt_resumed`).

**The sync test** (`--synctest N[,P]`, game_machine.c). Every P vertical
blanks: hash the machine at a frame's end (every 4 KiB page of every memory
area, and each part of the models' state), run N frames, hash again, go
back N frames and check the hash is the first one ("put back"), then run
the N frames again with the controls the first run had, without the frame
function, and check the hash is the second one ("replay"). Any difference
is listed by memory area and address, or by the model's name.

**The writer it found.** The first run (N 8, P 60, 185 checks over a
15,000-frame fight) had every replay right, but once the put-back machine
differed from the saved one in one 4 KiB page of ELAN memory. The
interpreter's store-queue flush (`sh4_exec.inc`) wrote RAM through
`mem_page` itself, past the tracker. The game sends its display lists to
ELAN memory by store queue, and once the run has gone back the code around
the main loop runs on the interpreter, so a page could be written there
unseen. The write is the game's, so the replay made it again and matched.
The flush now calls `mem_track` first.

**Tested** in the headless KWin, the Linux build (gcc) on the desktop
(Ryzen AI Max+ 395), GPU renderer, the sound chip on with SDL's dummy
output, unpaced, the scripted two-player fight (`--press`) to vertical
blank 15,000. Rollback starts at vertical blank 2,387, when the game has
booted:

| Case | Checks | Put back wrong | Replayed wrong |
|---|---|---|---|
| `--synctest 8,60`, before the flush's fix | 185 | 1 (one ELAN page) | 0 |
| `--synctest 8,60` | 185, twice | 0 | 0 |
| `--synctest 30,95` | 100 | 0 | 0 |

Every run, rollback off or on, going back or not, ended at vertical blank
15,000 after the same 15,194,980,856 instructions, and the pictures show
the fight played as without rollback.

**What it costs** (vertical blanks 4,500 to 15,000; the machine's thread
a frame, the view's lists and transform included, from the log's 600-frame
averages; two runs each, alternated):

| | Off | `--rollback 8` |
|---|---|---|
| The machine's thread a frame | 4.33, 4.34 ms | 4.82, 5.16 ms |
| Keeping the models a frame | | 0.10 ms |
| Copying pages a frame | | 0.19 ms (61 pages, 4.0 MB) |
| Memory held | | 64 MB |

So keeping 8 frames costs about 0.5 to 0.8 ms a frame here, 11 to 19
percent; the copies are 0.3 ms of it, the rest the slow first stores and the
caches the 4 MB pushes out. A frame writes more 64 KiB pages (4.0 MB) than
change (2.9 MB): a page counts when it is stored to, whether or not the
bytes change. 30 frames hold 220 MB. Going back 8 frames takes 1.5 ms
(158 times); running the 8 frames again takes 41 ms (median; 76 ms at
most), about 5 ms a frame: a frame run again costs what a frame costs,
the view's work included, since nothing is skipped yet.

**What it means for the two-PC game.** At 60 frames a second the desktop
has about 11 ms a frame to spare, two frames run again a frame. On the
Steam Deck, about 2.5 times slower (section 45's estimate), the frame
alone takes most of the 16.7 ms and there is no room to run frames again.
The next steps are to run frames again without the view (section 56:
lists, transform and the passes into textures: what they make is only
seen, and the game never reads texture memory, but a texture pass of a
frame run again would be missing from the next picture), to see what the slow first
stores cost under Proton, and the network side: guessing the other
player's controls, sending them, and going back when a guess was wrong.

Not covered: the Windows build was compiled (llvm-mingw), not run; the
sync test was not run under Proton or on a Deck. Sound made by frames run
again goes to the audio buffer as well (the test only); the two-PC game
will have to leave it out (section 56 does). `tests/lockstep` was not run: it builds
VF4debug's own interpreter, not `runtime/machine`, and nothing in
`runtime/cpu` changed.

## 56. Frames run again without sound or screens (2026-11-16)

Section 55's frames run again made their sound a second time and drew
their screens a second time: the sound went into the audio buffer after
the first run's, and every screen's lists were read, transformed and
drawn (or handed to the GPU) although no one would see them. Both were
seen and heard the first time. This section leaves them out, before any
network code.

**Sound** (`aica.c`). The chip runs in a frame run again as in any other
frame: its channels, envelopes, DSP and timers are machine state, and the
replay must leave them as the first run did. Only the last step, the
sample going into the audio buffer (which is not machine state), is left
out while `aica_audio_drop` is on. The sync test turns it on as the
machine goes back and off at the end of the last frame run again, and
checks that the samples dropped are as many as the first run made in
those frames and that none reached the buffer. The next sample after the
replay is made from the machine the first run had at the same point, so
what is heard is what would have been heard without going back.

**When a screen's lists are drawn.** The view (`view.c`) reads a screen's
display lists as the game sends them, from the frame's "start lists"
(core register 0x144) on, and draws the scene at the "start render"
(0x014). Measured over the first 8,000 vertical blanks (a temporary log):
of 7,057 screens, 7,047 were drawn in the frame after the one whose lists
they are: the game builds frame f's lists, and the render that frame
starts is of frame f-1's. The other ten were at the boot (two drawn in
the frame their lists began) and at loading breaks after the fight
starts and between rounds (lists begun, then nothing drawn for 2 to 151
frames). No pass into a texture was made in those 8,000 frames.

So the last frame run again is special: its lists are the ones the first
frame after the replay draws. `view_rerun` has three modes:

- `VIEW_RERUN` (from going back to the end of the next-to-last frame run
  again): a screen's lists are not taken (no lists read, no transform),
  and no screen is drawn.
- `VIEW_RERUN_LAST` (the last frame run again): its lists are taken, as in
  any frame; still no screen drawn.
- `VIEW_LIVE`: as ever.

No screen is drawn until the replay is over because the first render
after going back is of the frame before it, whose lists the view no
longer holds: its scene, found by the object list base, holds lists from
later in the first run. A pass into a texture is taken and drawn in every
mode, since its picture goes into texture memory, part of the machine,
and the screens after it use it. Going back also makes the texture cache
look at every texture again (`gen`): texture memory changed behind it.

**Tested** in the headless KWin as in section 55 (GPU renderer, sound chip
on with SDL's dummy output, unpaced, the scripted fight), to vertical
blank 12,100, each case against a run without rollback. A temporary
running sum of every sample put in the audio buffer was logged every 600
vertical blanks. The periods were chosen so that in every case a replay
ends at vertical blanks 3,027, 6,099, 9,043 and 11,987; the pictures
(`--shot`) were taken at 3,028 and 3,029, 6,099 and 6,100, 9,044 and
9,045, 11,988 and 12,000.

| Case | Replays | Put back or replayed wrong | Sound wrong | Sound sums (20) | Pictures (8) |
|---|---|---|---|---|---|
| `--synctest 1,63` | 151 | 0 | 0 | the same | the same |
| `--synctest 8,120` | 75 | 0 | 0 | the same | the same |
| `--synctest 30,98` | 75 | 0 | 0 | the same | the same |
| `--synctest 8,120`, software renderer | 75 | 0 | 0 | the same | the same |

"Sound wrong" counts replays that kept a sample, or dropped a different
number than the first run made (736, 5,888 and 22,048 samples, 735 or 736
a frame). The pictures were compared pixel by pixel: 3,028, 6,100, 9,044
and 11,988 are the first pictures drawn after a replay, from the lists of
its last frame. In the run without rollback a picture and the next differ
in 0.6 to 1.2 million pixels, so a picture left over or drawn from the
wrong lists would have shown. The software renderer's case (the view
draws the screen itself, into the picture the window shows) was compared
with a software-renderer run without rollback. The GPU renderer's run
without rollback took 4.33 ms a frame, as in section 55: frames run once
are unchanged.

**What it costs** (the time to run the frames again, from going back to
the end of the last one; the 8,120 and 30,98 cases):

| | Section 55 (view's work included) | Now |
|---|---|---|
| 8 frames again, median | 40.8 ms | 20.0 ms (10.0 to 28.9 ms, 10th to 90th percentile) |
| 8 frames again, most | 75.8 ms | 78.3 ms |
| 30 frames again, median | | 75.2 ms |
| A frame run again | about 5 ms | about 2.5 ms |

A frame run again now costs about what the machine's own work costs
(section 55 measured 2.28 ms) and its page tracking, whichever renderer:
with the software renderer, whose frames take 8.56 ms without rollback,
8 frames again took 20.7 ms (median). The slowest, 78 ms,
was in the loading break between rounds (vertical blanks 7,500 to 7,507);
late in the fight, where the machine's own work is heavier (6.2 ms a frame
without rollback), 8 frames again took about 40 ms.

**What it means for the two-PC game.** On the desktop, with about 12 ms a
frame to spare, four or five frames can be run again within one frame.
On the Steam Deck, by section 45's estimate of 2.5 times slower, a frame
run again would take about 6 ms with about 4 ms to spare (a frame and
keeping it, 12 to 13 ms): going back still costs the Deck frames. It
needs measuring on a Deck before the network side is built around it.
The network side is next: guessing the other player's controls, sending
them, and going back when a guess was wrong.

Also fixed, in section 55's sync test: had the run been unable to go back
(`rt_native_rewind` returning), the machine would already have been put
back, and the test would have stayed in its replay, no longer calling the
frame function and reading controls past the 60 it keeps. It now asks
`rt_native_can_rewind` before putting the machine back. It did not happen
in any test.

Not covered: no pass into a texture was made in the fight, so drawing
them in frames run again is untested; a pass whose lists began in the
frame before the one gone back to would be drawn from lists the view no
longer holds (as a screen's would, which is why no screen is drawn). A
scene whose lists begin in a frame run again (not the last) and that is
drawn more than a frame later, as at a loading break, is not drawn: the
picture stays as it was for that frame. The Windows build and Proton were
not run.

## 57. Latency optimization: guessing the other player's controls (2026-11-16)

Until now the two-PC game waited (section 46): each PC runs vertical blank
f only once the other's controls for f have come, and the input delay (2
vertical blanks, 33 ms) is all the time they have to come in. On a LAN
they take a millisecond or less. Over Wi-Fi with others on it, a VPN or
the internet they can take longer than 33 ms, or vary, and then the game
stops on both PCs until they come. This section adds the other way,
rollback, as a setting: **Latency optimization**, Auto, On or Off.

**On**, a PC that does not have the other's controls for the next vertical
blank does not wait: it guesses them (the newest of theirs that came,
repeated: in a fight a player holds a direction or a button for many
vertical blanks), and runs on, up to 8 vertical blanks past the newest
that came. When they come, they are compared with the guesses; if one was
wrong, at the end of the vertical blank it is running then the machine
goes back to the end of the vertical blank before the first wrong guess
and runs the ones since again with the real controls (sections 55 and
56: no screen drawn, no
sound kept, so the player sees and hears only the corrected run from then
on). **Off**, it waits, as before. **Auto** (the default) turns it on
while the connection needs it (below).

Each PC chooses for itself. The two runs are the same whatever each
chose: a PC's run is the vertical blanks it settled, all with both
players' real controls, and a vertical blank that ran on a guess is either
the same as its settled run or is run again. So a Steam Deck can keep it
off (it waits, as before) while the PC it plays keeps it on.

**Measuring the connection.** Each PC sends its controls every vertical
blank (section 46), so the controls packet carries the measuring: when it
was sent (ms, 16 bits), the newest send time of the other's it has had
(echo), and how long it held that before this send. A PC that gets its own
time echoed back takes the round trip as now − echo − held, and keeps a
smoothed round trip and how much it varies the way TCP does (RFC 6298),
with gains of 1/32 and 1/16 since 60 samples come a second, not one. The
packet also says whether its PC is guessing, and how late the other's
controls come there (below): 8 bytes more in each controls packet. The
two PCs must have the same build, as before.

**Auto.** The input delay hides delay × 16.7 ms of the way there, less
3 ms for the network thread's turn and the vertical blank's own work. Auto
turns latency optimization on once half the round trip plus twice its
variation has been more than that for a second (60 vertical blanks, after
at least 50 samples), and off once it has been 5 ms less for ten seconds.
With the input delay of 2: on above 30.3 ms, off below 25.3 ms. On a LAN
(round trip about 0.1 ms) it stays off. Each switch is in `vf4ft.log`.

**Where it is set.** The launcher's Settings, Game tab, and `latency` in
`settings.txt` (auto, on, off). The game's menu has the row only during a
two-PC game (Game tab), where its note says what it is doing now and the
round trip measured, and a change takes effect at once. A one-PC
release's Game tab is already ten rows, as many as it shows without
scrolling; a two-PC game's has no Test or Restart rows.

**What waits for the real controls.** Everything that leaves the machine
or is compared waits until every vertical blank before it ran with both
players' real controls ("settled"):

- the fingerprints (section 46): taken at the start of a check vertical
  blank as before, held until the vertical blanks before it are settled,
  and taken again if it is run again; only then sent and compared;
- the cards: a player's card is written from the newest check both PCs
  agreed on, as before (its copy is taken with the fingerprint, so again
  when run again);
- the host's backup (written every 10 seconds when it has changed): taken
  at the start of the vertical blank as before, written once the vertical
  blanks before it are settled, taken again if it is run again.

**How far it can go back.** The machine keeps the last 10 vertical blanks
(the guessing limit and 2). A guess is wrong at the earliest at the oldest
unsettled vertical blank, which is at least f − 8 when this PC is at f (it
runs no further than 8 past the newest of theirs, and those up to the
newest are settled once compared), and going back needs the end of the one
before it, f − 9: kept. A PC starts guessing only once its machine keeps
the vertical blanks since the newest of theirs; one whose machine cannot
keep them (no memory for them) says so and waits instead, as with Off.
Should going back fail all the same, the session ends with a message on
both PCs, as when the fingerprints differ (it did not happen in any
test). A PC that waits keeps nothing.

**Keeping the two PCs in step.** With both guessing, nothing holds a PC
back until it is 8 vertical blanks ahead, and the one that started first
would run on ahead of the other by as much as it started earlier: it would
guess every vertical blank and the other none. Each PC measures how late
the other's controls come against when it began their vertical blank
(smoothed), and sends it in the controls packet. The one that sees them
later than the other does is ahead: it makes each vertical blank later by
up to 1 ms (a sixteenth of the difference) until both see them equally
late. Only when both guess: with one waiting, the waiting PC is the one
held back already, and slowing the other would slow both.

**Tested** (the headless KWin, both games Linux, over loopback, the GPU
renderer, sound chip on with SDL's dummy output). `--net-lag MS,JITTER`
(for tests) sends each of a PC's datagrams MS ms later, give or take
JITTER, so the round trip is about twice MS.

**The same run whatever each chose.** Unpaced (as fast as each can go),
20,000 vertical blanks, a fingerprint in the log every 500; each case's
host and joining game against a one-PC run of the same presses moved on
by the input delay (the reference). "Run again": fingerprint lines logged
again for a check vertical blank run again (its first run's was from a
guess).

| Host / joining | Lag each way | Presses | Fingerprints the same (of 39) | Checks agreed | Went back every 600 | Run again |
|---|---|---|---|---|---|---|
| Off / Off | none | a few | 39, 39 | 333 | none | 0, 0 |
| On / On | none | a few | 39, 39 | 333 | 0 to 3 | 0, 0 |
| On / On | 30 ± 10 ms | the scripted fight | 39, 39 | 333 | 40 to 66 | 11, 16 |
| On / Off | 30 ± 10 ms | the scripted fight | 39, 39 | 333 | about 40 (the host) | 13, 0 |

The scripted fight is section 55's two-player fight (`--press`): its
player 1's presses the host's, its player 2's the joining game's. Player 1 changes
its controls every 10 vertical blanks and player 2 every 23, so a guess
of "the same as last" is wrong often: with 30 ms each way, almost every
vertical blank ran on a guess (585 to 600 of 600), and the machine went
back 40 to 66 times in 600, up to 9 vertical blanks at once. A wrong
guess at a check vertical blank showed as a different first fingerprint
and the right one when run again (vertical blank 19,000 on the host of
the On / Off case: `c4381c8e...` first, `603747f8...` run again, the
joining game's and the reference's).

**Joining a game going on** (section 49's test, all three games On, 30 ±
10 ms each way, the join code typed): A hosts, B plays to vertical blank
9,000 and leaves, A plays alone, C joins A's game going on at 10,409 and
plays to 36,000. A's fingerprints and B's agreed at all 18 they share,
A's and C's at all 56 (C's from before the join are of its copy of the
session's start); both sessions went back 3 to 7 times every 600 vertical
blanks.

**At the game's own pace** (both Auto unless said, the scripted fight,
to vertical blank 9,000; all fingerprints the same as the reference's in
every case, 17 of 17, and 150 checks agreed):

| Lag each way | Choice | Auto | Guessed, every 600 | Went back, every 600 (most at once) | Waited, every 600 | Controls came, host / joining |
|---|---|---|---|---|---|---|
| none | Auto | stayed off | 0 | 0 | 0 | 14.5 / 52 ms early |
| 30 ± 10 ms | Off | | 0 | 0 | host 8 to 31 (21 to 64 ms); joining 223 to 257 (1.09 to 1.33 s) | 9 early / on time |
| 30 ± 10 ms | Auto | on at the 59th vertical blank | 105 to 146 | 6 to 33 (2) | none after it turned on | 5 / 5 ms early |
| 60 ± 15 ms | Auto | on at the 59th | 516 to 600 | 52 to 121 (5) | none after it turned on | 23 / 23 ms late |

With no lag Auto stayed off (round trip 0.1 to 0.3 ms) and the game
played as before: the other's controls came 14.5 ms before they were
needed on the host and 52 ms on the joining game (together twice the
input delay, 66.7 ms; the joining game ran 19 ms behind the host), and
neither waited. With 30 ± 10 ms each way
both PCs turned it on 59 vertical blanks after the start (the
round trip measured 60 to 61 ms, varying by 7 to 8: half of it and twice
the variation is 45 ms, more than the 30.3 the input delay hides). The
controls then came 5 ms early on average on both PCs (each 600 vertical
blanks, both within 1.5 ms of each other: the two kept in step), but up
to 18 ms late now and then, so about one vertical blank in five ran on a
guess, never more than 2 in a row, and the machine went back 6 to 33
times in 600, 2 vertical blanks each time. With Off instead, the same lag
stopped the joining game 223 to 257 times in every 600 vertical blanks,
1.1 to 1.3 seconds in every ten: in lockstep one PC runs at the edge,
where any packet later than the average stops it.

With 60 ± 15 ms each way (a round trip of 120 ms, far more than the
input delay hides) nearly every vertical blank ran on a guess, and the
machine went back 52 times in 600 on the host and 120 on the joining
game (which had more of the other's changes to put right), at most 5
vertical blanks at once. The machine's time a frame (the desktop) went
from 5 to 7 ms on average in the fight to 7 to 11 ms, at most 48 ms;
once Auto had turned on, neither game fell behind by a quarter of a
second (the joining game did once in the first second, waiting, before
it turned on).

**At Steam Deck speed** (imitated: no Deck was used). The joining game's
threads each limited to 35 or 45% of one core (a cgroup's `cpu.max`, on
4 cores and their SMT siblings), the earlier estimates of the LCD Deck's
and the OLED's speed; the host at full speed on the other 12 cores. Both
at the game's own pace, 30 ± 10 ms each way, the host On, the scripted
fight to vertical blank 9,000. "In the fight": vertical blanks 4,788 to
6,588, the heaviest part.

| Joining game | Its choice | Vertical blanks a second, in the fight / from 2,988 on | Its machine's time a frame in the fight | Behind by 1/4 s | It went back, every 600 |
|---|---|---|---|---|---|
| full speed | On | 60 / 60.0 | 6.3 to 7.8 ms | never | 15 to 27, 2 at once |
| 45% (OLED) | Off | 60 / 60.0 | 10.7 to 11.0 ms | never | none; it waited 13 to 98 times every 600 (24 to 318 ms) |
| 45% (OLED) | On | 59.9 / 59.9 | 14.4 to 14.5 ms | never | 27 to 49, 2 or 3 at once, 14 to 20 ms each time on average |
| 35% (LCD) | Off | 53.8 to 56.4 / 58.2 | 17.5 to 18.3 ms | 11 times | none |
| 35% (LCD) | On | 51.5 to 53.8 / 55.9 | 18.4 to 19.1 ms | 24 times | none in the fight; 41 to 52 in the break after it, up to 8 at once, 18 to 24 ms each time on average |

At the OLED's speed latency optimization held the full pace with no
waiting (Off waited, as in lockstep), but a vertical blank that went back
took up to 52 ms with the frames run again in it. At the LCD Deck's speed
the joining game's own vertical blanks take more than 1/60 s in the fight
whichever it chose, so both games ran at its pace there (52 to 56 a
second); On cost it about 1 ms a vertical blank more (keeping its frames)
and slowed it further. In the fight it never guessed: it was the one
behind, so the host's controls were always there; the host did the
guessing. The host (On) then ran up to 8 vertical blanks ahead of it,
went back 52 times every 600, 9 vertical blanks each time, and waited 113
to 163 times every 600, whichever the joining game chose: the game can go
no faster than the slower PC, and the host's guessing only ran ahead and
put it right. Keeping the two in step (above) works only while both
guess, and by at most 1 ms a vertical blank, too little for a PC 15%
slower. So on a Deck that slows down, Off is better for the Deck; making
the faster PC follow a slower one, and keeping frames for less, are left
for later.

The imitation's numbers depend on how it is run: over vertical blanks
4,500 to 7,000, the joining game Off took 16.4 ms a frame at 35% and 10.3
at 45%, where the earlier one-PC run under the same limits (unpaced, no
session) took 13.7 and 9.7. A first run of these cases with the host
free to use any core gave the same as the table, within 1%. How far
either is from a real Deck is not known.

Also fixed: the game's pacing took the time before the two-PC game's wait,
so after waiting for the other's controls it slept a full vertical blank
more than it should have, from a time already past. It takes the time
again after the wait now. In a two-PC game `--rollback` and `--synctest`
are refused: the session goes back itself.

Not covered here: the Windows build (the release check runs it under
Wine against the Linux build); a real Deck; a real slow connection (only
the loopback with `--net-lag`); losing datagrams (`--net-lag`'s third
number), which the controls' resending covers as before.

## 58. The game's pace from the screen's refresh (2026-11-17)

A report (issue 1: Windows 10, an i9-12900K, an RTX 3080 Ti): "Game is
smooth and stable until it drops to half frame rate. Feels like a VSYNC
issue." Asked, the reporter added: choppy with no loss of game speed, at
random and most often in the first seconds of a match, on a screen at
just under 60 Hz; and sent the game's log.

**The fault** is ours. The machine drew 60 frames a second by its own
timer (`FRAME_NS`, section 44), and the window presents on the screen's
refresh, each present taking the newest frame. Nothing tied the two. No
screen refreshes at exactly the timer's 60: the moment a frame is handed
over slides slowly against the moment a present takes one, and while the
two are close, small differences in how long a frame took decide whether
it makes its present. Then some frames are shown twice and others never.
The slower the slide, the longer each such stretch and the longer between
them.

**Measured** (the headless KWin, the GPU renderer, the scripted fight; a
test-only record of when each frame was handed over and which frame each
present showed, `--sync-log`). KWin's virtual screen refreshes at exactly
60 Hz by the same clock as the game's timer (it takes no other rate from
its output config), so the slide was made by running the game's timer a
little fast (`--pace-ppm`), which is the same thing seen from the game:

| The game's timer against the screen | Frames never shown | How |
|---|---|---|
| the same (no slide) | 0.13% | around loading only |
| 1,000 parts in a million fast (as a 59.94 Hz screen) | 1.0% | one or two seconds about every 17 s; the worst second showed 51 of 60 |
| 100 parts in a million fast (as a 60.006 Hz screen) | 1.3% | stretches of 15 to 35 s losing 10 to 20% of the frames, about every 167 s; the worst second showed 45 of 61 |

For every frame never shown another is shown twice. The machine itself
never fell behind (3 to 7 ms a frame). On this desktop a frame's time
varies by about 1 ms from one to the next; a PC where it varies more
loses more in each stretch.

**The reporter's log** (0.5.11, a window on a screen SDL gives as 59.99
Hz; presents came at 59.98 a second) fits: the machine took 6 to 7.5 ms a
frame in fights and was never behind in 17 minutes, and the game's own
frame count went up by one every vertical blank in the matches, so the
game drew every frame. But the longest frame in most 10-second spans was
13 to 17 ms, far more varied than here, and of about 59,900 frames the
game ran the renderer drew 58,640: up to 2% never shown (frames while
loading are in that count too). That log could not say more: it had no
count of frames shown.

**The fix: the machine follows the screen** (`runtime/host/vsync.c`), when
the screen refreshes at 60 Hz or a multiple of it.

- *What the screen does.* The window's thread tells `vsync_take` the
  moment each present takes its frame (the GPU renderer's after the
  swapchain's wait; the software picture's where it is copied). From
  those it keeps the refresh's period and phase. The first period is from
  the first 16 intervals (their median, then all of them over the
  refreshes they span by it). Each take is then compared with where the
  period says it should be. One that fits (within a quarter of a refresh,
  up to 4 refreshes after the one before) moves the phase by an eighth of
  its error. The period is first moved by the error too (a 64th), then,
  once the run of takes that fit spans half a second, measured across the
  run (up to the last 512 takes), with no take's error counted for more
  than 0.5 ms. So one present held up (KWin's virtual screen delivers one
  about 3 ms late every 6 seconds or so) hardly moves it; with the
  period moved by each error alone, those knocked it past the limit below
  every few seconds. A take that does not fit changes nothing by itself
  (the grid goes on by its period: a present missed, a stall); 3 in a row
  start the phase again, and 16 the period.
- *When it is followed.* Once takes have fitted for a second, the newest
  is no more than 8 refreshes old, and k refreshes (k a whole number) are
  within 0.25% of the machine's 1/60 s (0.5% once followed): 59.85 to
  60.15 Hz, and the same around 120, 180 and 240. So 59.94 Hz is followed
  (the game then runs 0.1% slow, sound and all, as a console does on such
  a screen; the sound's rate already bends by up to 0.5% to keep its
  queue, section 16), and 144, 90, 75 and 50 Hz are not.
- *How.* The machine's thread asks `vsync_next` once a frame for when the
  next is due. The period is the screen's (k refreshes), and the phase is
  set so that a frame is *handed over* halfway between two takes: as far
  from both as it can be. How long after its start a frame is handed over
  differs by PC and by scene (under 1 ms in menus, 4 to 10 ms in a fight
  here, 13 to 17 ms at most on the reporter's PC), so main.c keeps the
  highest and the lowest of late (each let go over about 8 seconds) and
  gives their middle (`lead`). A first version started each frame a fixed
  2 ms after a take instead: in the model below it lost 3.7% of the frames
  of a PC that takes 12 ms a frame, give or take 1.5, where this loses
  none. What is left of the phase's error goes a quarter a frame, at most
  0.5 ms a frame, or all at once when the pace starts again (after the
  menu, a pause, or falling far behind).
- *Several refreshes to a frame.* On a 120 Hz screen (k = 2) two presents
  come to a frame. Only the first of each two takes a new frame
  (`vsync_take` says which); the other shows the same one again. So a
  frame handed over at any time in those two refreshes is shown for
  exactly two, not for one or three.
- *Otherwise the timer, as before:* a screen at another rate (the refresh
  notice of section 44 still says to set 60 Hz); presents that are not
  steady; nothing presented (the window minimized or hidden: the game and
  its sound go on); a two-PC game, where both PCs must go at one rate
  (section 46) and the session does its own pacing (section 57); and
  `pace = timer` in `settings.txt` (the default is `screen`; the setting
  is in the file only, for a PC where following goes wrong). Going from
  one to the other needs no step: the next frame is due from the last one
  either way. A screen whose rate changes while the game runs is measured
  again (the takes stop fitting), and the window moving to another screen
  starts the measuring again.

Pacing never changes the run: the same controls give the same game.

**The log** says what paces the machine when it changes (`pace: vertical
blank N: by the screen's refresh (59.940 Hz)`, or `by the game's own
timer (...)` with why), and with the machine's line every 600 vertical
blanks: `screen: 600 frames drawn, 0 of them never shown; 600 presents;
paced by ...`. A frame is counted never shown when the next one reached the
window's thread before a present had taken it. So a report like issue 1 can
be read from `vf4ft.log`.

**Checked: a model** (not in the repository): a screen whose presents
take the newest frame at each refresh, and a machine that works on a frame,
hands it over and paces itself with `vsync_next` as main.c does, both
around the real `vsync.c`; 10 minutes each, a frame taking 6 ms give or
take 1 unless said, takes 0.3 ms uneven.

| Screen | By the timer (before) | Following |
|---|---|---|
| 60.006 Hz | 1,078 of 36,000 frames never shown | 0; followed from 1.3 s on |
| 59.94 Hz | 1,286 | 0 |
| 60.000, 60.1 Hz | | 0 |
| 59.94 Hz, a frame 0.7 ms (menus) | | 0 |
| 60 Hz, takes 1.5 ms uneven | | 0 |
| 60 Hz, a present 3 or 5 ms late every 342 | | 0 |
| 60 Hz, a frame 12 ms give or take 1.5 | | 19, all at the start |
| 60 Hz, a frame 15 ms give or take 2 (some longer than a refresh) | | 121 |
| 60 Hz, a present missed now and then | | one frame for each missed present, no more |
| 120, 240 Hz | | 0; every frame shown for 2, or for 4, refreshes |
| 120 Hz, a frame 12 ms give or take 1.5 | | 0 |
| 144, 90, 75, 61, 50, 30 Hz | not followed: as before | |

A screen that changes rate while the game runs (the same model, the
change at 100 s): 60 to 120 Hz and 60 to 59.94 Hz, no frame lost; 120 to
60 Hz, 18 lost in the change and 240 to 60 Hz, 24; 60 or 120 to 144 Hz,
none lost and the timer from then on; 144 to 60 Hz, followed 1.5 s after
the change.

**Checked: the game** (the scripted fight; the log's own counts unless
said; "frames" are those the game drew from vertical blank 4,500 on):

| Screen | Renderer | The game's timer | By the timer (`pace = timer`) | Following |
|---|---|---|---|---|
| KWin, 60 Hz | GPU | 100 ppm fast, to vertical blank 24,000 | 216 of 18,823 frames never shown (1.1%) | 3 of 18,880; the machine at 60.0004 a second, the screen's |
| KWin, 60 Hz | GPU | 1,000 ppm fast, to 16,000 | 93 of 11,014 | 1 of 11,003 |
| KWin, 60 Hz | software | 100 ppm fast, to 20,000 | 283 of 17,400 | 9 of 17,400 |
| gamescope, 60 Hz (X11) | GPU | 100 ppm fast, to 9,000 | | 0 of 4,333; shown for one present 99.93% |
| gamescope, 120 Hz | GPU | 100 ppm fast, to 14,000 | 0 of 9,179; shown for 1, 2, 3 presents: 1.5%, 97.0%, 1.5% | 2 of 9,179; for 2 presents: 99.89% |
| gamescope, 144 Hz | GPU | 100 ppm fast, to 9,000 | | not followed: for 2 or 3 presents, 60% and 40%, as before |

("Frames never shown" in the first two rows is frames drawn less frames
shown in the sync log; the rest are the log's counts.) With the first
kind of period (moved by each take's error alone) the same two fights
lost 1 and 1 frames but went back to the timer for 2 seconds every 6 or
so, as said above; with the period measured across the run, the 24,000
vertical blank fight was followed from the start to the end without a
break, and the 16,000 one but for 2 seconds once.

- *The same run.* The machine's fingerprints (`--hash-every 2000`) of the
  1,000 ppm fight following and by the timer are the same, all 8, and the
  100 ppm one's first 8 the same again.
- *A two-PC game* (both latency optimization On, 30 ms give or take 10
  each way, to vertical blank 9,000): both logs say `by the game's own
  timer (a two-PC game)`; 149 checks agreed, and the two logs' 18
  fingerprints are the same. Its frames are still lost as before (23 and
  34 in 600 at worst here): following the screen in a two-PC game is not
  done.
- *The Windows build* under plain Wine (the headless KWin through Xwayland,
  a test prefix with no sound device), the timer 100 ppm fast, to vertical
  blank 9,000: followed from the start (`by the screen's refresh`), 0 of
  4,333 frames never shown, each shown for one present (99.93%).
- *The builder*: a test package's builder built the Linux game with
  `vsync.c` in it (126 files; its log line is in the game).

Not covered: Windows itself and a real screen (every run here was on a
virtual screen whose clock is the game's, the slide made with
`--pace-ppm`); the reporter's PC; a screen changing rate while the real
game runs (the model only); G-Sync or FreeSync; a Steam Deck. (gamescope's
headless backend crashed each time as it exited, after the game had ended
cleanly; not looked into.)

## 59. Frame pacing: the game by its own clock (2026-11-17)

Section 58 made the game follow the screen's refresh. That ties the game's
speed to the screen (a quarter of a percent at most, but each PC's own),
so it cannot be used in a two-PC game, where it left the old uneven
stretches; and the point of the machine having its own thread and timer
was that the game does not depend on the screen. So the other way round is
now the default: **the game keeps its own clock, and each frame is put on
a refresh of the screen.**

**What cannot be had.** A game drawing exactly 60 frames a second on a
screen showing 59.94 has 0.06 of a frame too many each second: about every
17 seconds one frame is not shown. On a screen a little fast, one is shown
twice now and then. No pacing avoids that one frame while the game keeps
its own time. What was wrong before section 58 was not that frame but the
stretch around it: as a frame's hand-over came close to the moment a
present took one, small differences in how long each frame took decided
which were shown, and for seconds or minutes some were shown twice and
others never.

**How** (`vsync_pick` in `runtime/host/vsync.c`).

- Each frame the machine hands over says when it is to be shown by the
  game's clock: when it was due (the timer's own time, steady whatever the
  frame took), plus the longest a frame has taken to be handed over of
  late (four fifths of a frame at most), plus 2 ms. By then it has come.
- The window's thread knows the screen's refreshes as section 58's filter
  keeps them (the period, the phase, a count). A frame's refresh is the
  one given to the frame before it, plus the whole number of refreshes its
  time is on from that frame's (1 at 60 Hz, 2 at 120 Hz, 2 or 3 in turn at
  144 Hz). So frames go on the screen at even steps, whatever each took.
- A frame keeps that step until its refresh would come more than 0.75 ms
  before its time (then it moves one refresh later: at 60 Hz the frame
  before is shown once more) or more than a refresh and 0.75 ms after it
  (one sooner: the frame before is passed over). With the game a little
  fast or slow against the screen, that happens once each time the two
  have gone a whole refresh apart, and nowhere else: between two such
  steps every frame is shown for the same number of refreshes.
- At each present, the newest frame whose refresh has come is shown. One
  that came in but is not due waits, so two can wait at once: the GPU
  renderer keeps two frames (each with its own share of the texture
  uploads, so a frame is drawn with the textures as they were at its own
  time), and the software picture two pictures. A third puts the oldest
  out.
- Without a steady refresh (the first second, a screen changing its mode,
  nothing presented) it is the newest frame at each present, as before.

It needs nothing of the screen's rate: 60, 59.94, 120, 144, 90 and 75 Hz
go the same way (at 144 Hz a frame is shown for 2 or 3 refreshes in a
fixed turn). It is the same in a two-PC game, where the machine's pace is
the session's.

**The price** is time: a frame waits for its own refresh, not the next
one after it came. In the model below a frame is on the screen 13.4 ms
after it was handed over on average at 60 Hz, where the newest frame at
each present gives 8.0 ms (and section 58's following 8.3).

**The setting** is Frame pacing, on the Picture tab of the game's menu
(applies at once) and of the launcher's Settings, `pacing` in
`settings.txt`:

- **Game clock** (`game`, the default): this section.
- **Screen refresh** (`screen`): section 58, the game following a screen at
  60 Hz or a multiple of it.
- **Off** (`off`): the newest frame at each present, as before 0.5.12.

**A two-PC game goes by the host's choice**, on both PCs, so that both
players' pictures reach their screens the same way and as late. Each PC
says its own Frame pacing with its controls (two bits of the INPUT
datagram's flags, which had room; the two PCs are the same build anyway),
and the joining PC goes by the host's from the first one it hears (by its
own for the moment before). Where the host chose Screen refresh both use
Game clock: the two machines must keep one time, the session's. The row's
note in the menu says which is in use, and that the joining PC's own choice
is kept for its own games.

0.5.12's `pace` is read once more: `timer` there (a choice someone made)
is Off; `screen` was only its default, and gives way to the new default.
The launcher's Picture tab has nine rows with it, set a little closer (37
units, not 42) so that all nine show without scrolling.

The log's `pace:` line and the `screen:` line every 600 vertical blanks say
which it is: `the game's own clock, each frame put on a refresh of the
screen (59.940 Hz): 1 moved a refresh sooner, 0 later`.

Also in `vsync.c`, for both ways: presents that do not fit the refresh's
grid are now weighed (one that does not fit counts 2, one that fits takes
1 off; at 6 the phase starts again: three in a row as before, or most of a
dozen), so a few uneven presents among many even ones start nothing again;
and after the phase has started again the period, measured before, is
trusted after 8 presents that fit, not a second. On KWin's virtual screen
the phase jumps by about half a refresh now and then (one interval of 6 to
9 ms): each jump put the pace back to the newest frame for a second, twice
in a 2-minute fight; after the change, for the 8 presents.

**Checked: the model** (`vsync.c` between a machine by its own timer and a
screen; 10 minutes each, the counts from the second 5; a frame taking 6 ms
give or take 1, presents 0.3 ms uneven, unless said). "Before": the newest
frame at each present.

| Screen | Before: never shown, shown twice | Game clock: never shown, shown twice | Hand-over to screen |
|---|---|---|---|
| 59.94 Hz | 608, 590 | 18, 0 (one every 16.7 s: what cannot be had) | 13.4 ms (before: 8.0) |
| 59.98 Hz | 627, 621 | 6, 0 | 13.4 ms |
| 60.000 Hz | | 0, 0 | 17.2 ms (wherever it started: it never moves) |
| 60.006 Hz | 685, 687 | 0, 2 | 13.0 ms |
| 60.1 Hz | | 0, 30 | 11.9 ms |
| 59.94 Hz, presents 1.5 ms uneven; one 3 ms late every 342 | | 18, 0 | 13.4 ms |
| 59.94 Hz, a frame 12 ms give or take 1.5 | | 19, 1 | 12.4 ms |
| 59.98 Hz, a frame 6 give or take 2 ms, every 37th 7 ms longer | 1,358, 1,351 | 9, 3 | 17.1 ms |
| the same, every 11th 9 ms longer | | 55, 49 | 17.4 ms |
| 120 Hz | shown for 1, 2, 3 refreshes: 1,211, 15,575, 1,214 | 0, 17,997, 3 | 8.5 ms (before: 4.2) |
| 119.88 Hz | | 35, 17,964, 0 | 9.2 ms |
| 240 Hz | | all for 4 refreshes but 6 | 6.3 ms |
| 144, 165, 90, 75 Hz | | for 2 and 3 (60%, 40%), 2 and 3 (25%, 75%), 1 and 2 (half each), 1 and 2 (75%, 25%), in a fixed turn | 7 to 11 ms |
| 50, 30 Hz | | 1 in 6 and 1 in 2 never shown: the screen has no room for them | |

A screen that changes rate (60 to 120, 120 to 59.94, 60 to 144 Hz at 100
s): the new rate is taken up and the counts after it are the new rate's
own.

**Checked: the game** (the scripted fight; "frames" are those the game drew
from vertical blank 4,500 on, from the sync log, unless said; the game's
timer set off the screen with `--pace-ppm`, as in section 58):

| Screen | Renderer | The game's timer | Frame pacing | Frames never shown | Shown for |
|---|---|---|---|---|---|
| KWin, 60 Hz | GPU | 1,000 ppm fast | Off | 57 of 7,179 | one present 99.21%, two 0.70% |
| KWin, 60 Hz | GPU | 1,000 ppm fast | Game clock | 8 of 7,179 (7.5 cannot be had in 125 s) | one present 99.92% |
| KWin, 60 Hz | GPU | 1,000 ppm slow | Game clock | 6 of 7,179 | one 99.75%, two 0.17% |
| KWin, 60 Hz | GPU | 100 ppm fast | Game clock | 2 of 7,179 | one 99.90% |
| KWin, 60 Hz | GPU | 100 ppm fast | Screen refresh | 0 of 7,179 | one 99.92% |
| KWin, 60 Hz | software | 1,000 ppm fast | Game clock | 12 of 9,000 (the log's count; 9 cannot be had) | |
| gamescope, 60 Hz (X11) | GPU | 1,000 ppm fast | Game clock | 5 of 4,333 (4.5 cannot be had) | one 99.93% |
| gamescope, 120 Hz | GPU | 1,000 ppm fast | Game clock | 0 of 4,333 | two 99.75%, one 0.18% (the steps) |
| gamescope, 144 Hz | GPU | 1,000 ppm fast | Game clock | 0 of 4,333 | two 60.18%, three 39.75% |
| KWin, 60 Hz, the Windows build under Wine | GPU | 100 ppm fast | Game clock | 0 of 4,333 | one 99.93% |

(The 1,000 ppm slow run went back to the newest frame for 2 seconds once,
at one of KWin's phase jumps, before the change above.)

Two-PC games (both latency optimization On, 30 ms give or take 10 each
way, both timers 1,000 ppm fast, to vertical blank 9,000; the joining
PC's menu opened once on the Frame pacing row):

| Host's choice | Joining PC's | Both logs say | Never shown in the last full 600 (host, joining) | Checks agreed |
|---|---|---|---|---|
| Game clock | Off | the game's own clock, each frame put on a refresh | 6, 5 | 150, 149; the 18 fingerprints the same |
| Off | Game clock | the game's own timer (a two-PC game) | 22, 20 | 149, 149; the same |
| Screen refresh | Off | the game's own clock, each frame put on a refresh | 2, 2 | 149, 149; the same |

The joining PC's menu showed its own Off on the row and the note "A
two-PC game uses the host's choice: Game clock now. Yours is kept for your
own games."

**Checked: a real screen** (with the maintainer's leave: this build's
Windows game under Proton on the desktop's own 3840 x 2160 screen at 59.94
Hz, HDMI, the GPU renderer at 4 x in a 3414 x 1920 window, sound off; the
attract sequence for two minutes; frames from vertical blank 3,000 on, 110
s). Presents there come evenly: half within 0.22 ms of the refresh's grid,
99% within 1.2 ms.

| Frame pacing | Frames never shown | Shown for | In each 10 s |
|---|---|---|---|
| Off | 75 of 6,590 | one present 98.91%, two 1.06% | 6, 0, 6, 13, 2, 2, 1, 16, 16, 0 |
| Game clock | 10 of 6,590 (6.6 cannot be had) | one present 99.91% | 0, 1, 1, 1, 1, 0, 3, 1, 1, 1 |

0.5.12's Screen refresh on the same screen (the release's own game, the
run before this section): followed from 3.7 s to the end, 3 frames never
shown in two minutes.

Not covered: Windows itself; the reporter's PC; the real screen at 120 Hz
(0.5.12 lost the screen four times in 20 s there in a run where the screen's
mode was being changed by hand at times not noted; not run again); a screen with G-Sync or
FreeSync (there the screen could follow the game, which is not done); a
Steam Deck; a screen changing rate while the real game runs (the model
only).

## 60. One device a GPU for the whole run (2026-11-17)

**What was seen.** The 0.5.13 release under Proton, played for eight
minutes on a 120 Hz screen, with Renderer and GPU changed in the menu on
the way (the GeForce, the Radeon, Software, the Radeon, Auto, the GeForce),
took 33 seconds to close after Quit. Its log has the machine stopped and
saved at once, and then 33.2 s between the last line of statistics and
"clean exit". Every run before, with no change in the menu, closed in 0.06
to 0.25 s.

**Where the time goes.** It was made to happen again in a headless KWin,
with no Steam: Proton Experimental and plain Wine 11.22, NVIDIA's driver
620.70.06, an RTX 5070 Ti. Each step of the closing was timed:

| Step | Usually | In the slow closing |
|---|---|---|
| the GPU has nothing left to do (SDL_WaitForGPUIdle) | 1 ms | 1 ms |
| SDL_ReleaseWindowFromGPUDevice | 20 ms | 22.0 s |
| SDL_DestroyGPUDevice | 60 ms | 11.2 s |

Wine's trace of the Vulkan calls (`WINEDEBUG=+timestamp,+vulkan`) names
them: `vkDestroySwapchainKHR` 22.01 s and `vkDestroyDevice` 11.15 s. The
game's main thread is not asleep in that time. It runs, inside NVIDIA's
`libnvidia-glcore`, reading the clock (two samples of it under gdb: one in
the clock's read, one in the driver's own code). The times are nearly the
same run after run (22.01 to 22.06 s and 11.16 to 11.22 s over six runs): a
wait that gives up after about 11 seconds, three times. What the driver
waits for is not to be seen from outside it.

**What brings it on.** The same steps by the game's `--keys` (the Renderer
row to Software and back, then Quit), changed one thing at a time:

| Case | The closing |
|---|---|
| Renderer to Software and back at vertical blank 3000 (the attract sequence's 3D scenes), Quit at 3040 or at 3400; Proton and plain Wine | 33.2 s |
| GPU to the Radeon at 3000 (it cannot show the window there, so a new device on the GeForce), Renderer changed at 3400 | 33.2 s, in the menu: the window stands still that long |
| the same with NVIDIA's shader cache on disk off (`__GL_SHADER_DISK_CACHE=0`) | 33.2 s |
| the same with 0.5.12 | 33.2 s |
| Renderer to Software and back at 400 (the boot's screens), Quit at 3040 | 0.15 s |
| started with Software, GPU at 3000 (the first device to draw with the GPU renderer, made in the middle of the game) | 0.09 s |
| Internal resolution 4x to 3x at 3000 (the renderer made again on the same device, every texture sent again) | 0.13 s |
| the Linux game, the same steps, on Wayland and on X11 through Xwayland | 0.15 s, 0.09 s |

So it takes a second device on the GeForce in one process, after a first
one that drew the game's 3D scenes and was closed. Closing the second one
stalls, at the quit or at the next change in the menu. Only under Wine.

It is not Steam's overlay (there was no Steam; in the maintainer's run
Steam did start an overlay process for each new device). It is not the
external GPU's link (the kernel's log has nothing from NVIDIA's driver or
PCIe at that time). It is not section 59's pacing (0.5.12 does the same;
Renderer and GPU have been changed at once since section 53).

**What is done.** No GPU gets a second device. A device that has shown the
window is kept open until the game ends (`held` in `runtime/host/main.c`):

- When Renderer or GPU changes, the device gives the window back
  (video_close: its swapchain is closed, which on a first device is quick
  each time) and is held.
- video_open takes a held device when its order comes to that GPU, in
  place of opening the GPU again. The log says "GPU: NAME: the device
  opened before is used again". The Software renderer's picture goes
  through a device as before, so GPU, Software and GPU again on one GPU is
  one device throughout.
- A device that never became the window's (it could not show the window,
  or was opened only to see which GPU SDL takes) is closed at once as
  before: it drew nothing.
- SDL gives a device's memory back as it submits a command buffer. After
  the window is given back an empty one is submitted, so the held device
  keeps none of what the renderer had. Measured on the GeForce, the Linux
  game at 4x with GPU changed to the Radeon (48 MiB in use with nothing
  running, 135 with the test's compositor): 532 MiB with the game drawing
  on it; after the change 259 MiB, and 531 MiB without the empty command
  buffer.
- At the end video_end closes the window's device and every held one.
- A step of a closing that takes a second or more is logged by its name,
  as "closing: giving the window back on NAME took 22.0 s", so a driver
  that stalls is found by the step.

**What it costs.** A GPU the game has drawn on stays open in the game
until it quits, also after the menu moved to another. An external GPU is
then still in use by the game. (A GPU that goes away as the game runs was
not handled before either.)

**Checked** in the headless KWin, this build, 3416 x 1920 in a 3414 x 1920
window, sound off:

| Case | Changes in the menu | The closing |
|---|---|---|
| plain Wine: Software and back at 3000, Quit at 3040 (33.2 s before) | 0.10 s, 0.06 s | 0.06 s |
| Proton: the maintainer's run (the Radeon, Software, GPU, Auto, the GeForce), Quit at 3900. Under Wine there the Radeon cannot show the window (section 53), so the GeForce's one device serves all of it | 0.06 to 0.12 s each | 0.08 s |
| the Linux game, the same: the GeForce and the Radeon both drew, each held while the other did | 0.01 to 0.11 s each | 0.17 s, both devices closed |
| the picture at vertical blank 3500 with Software and back at 3000, against a run with no change; the Linux game and plain Wine | | the four pictures are the same file, byte for byte |

**Checked: a real desktop** (with the maintainer's leave: this build's
Windows game under Proton on the desktop's own 3840 x 2160 screen at 59.94
Hz, sound off, the same changes by `--keys`, Quit at vertical blank 4850).
There Xwayland takes the Radeon's pictures, so both GPUs drew: the GeForce,
the Radeon (a second device, the GeForce's held), Software through the
GeForce's again, the Radeon's again, then Auto and the GeForce on its first
device still. Each change took 0.06 to 0.09 s, and the closing 0.13 s with
both devices closed. These are the changes that ended in the 33.2 s closing
with 0.5.13. In the last full count of 600 frames, on the GeForce again,
600 were drawn and none was never shown, in 599 presents.

**Not covered.** Windows itself (the stall is in NVIDIA's Linux driver;
whether Windows' has one was not tried). Other versions of NVIDIA's driver.
A screen at 120 Hz with this build: the 0.5.13 run that was slow to close
had about 90 presents a second, not 120, in its last 14 seconds, after its
last change of GPU; whether that came from the same state of the driver
was not looked into. The launcher, which makes its renderer again with
Renderer and GPU too: it draws only its pages, and a first device that
drew only the boot's screens did not bring the stall on, but its closing
was not timed. Why the driver waits: not found, and not reported to
NVIDIA.

## 61. The screen's rate for its notice, held against the presents (2026-11-17)

**What was seen.** A 0.5.14 game under Proton on a screen at 120 Hz logged
`screen: 60.00 Hz`. The pacing was right all the same (1,200 presents to
600 frames: sections 58 and 59 go by the presents they measure). But that
figure, the system's report (`SDL_GetCurrentDisplayMode`), is what section
44's notice and the Fullscreen row's note go by. A wrong one fails both
ways: a 144 or 90 Hz screen reported as 60 gets no notice, and a screen
the player has just set to a multiple of 60 Hz, as the notice told them
to, still gets it while the report says 144.

**Why the report was wrong.** The kernel's display log and Steam's log
give the times of two runs on the same PC:

| Run | The screen went to 120 Hz | Steam started the launcher | The game logged |
|---|---|---|---|
| 0.5.13 | 17:44:36 | 17:48:07, 3.5 minutes later | `screen: 120.00 Hz` |
| 0.5.14 | 18:56:51 | 18:56:39, 12.7 seconds earlier | `screen: 60.00 Hz` |

In the second the game itself started 85 seconds after the change and was
still told 60. So under Proton the report looks to be the rate the screen
had when the launcher's Wine session started. (Two runs; no test made the
change on purpose, and where the old figure is kept, Xwayland or Wine, was
not looked for.) A Steam Deck OLED's report was not trusted already
(section 44). This reason is wrong: section 62 has a third run that it
does not explain.

**What the presents can say, and what they cannot.** vsync.c keeps the
highest steady rate the presents have shown in the last ten seconds
(`vsync_screen_hz`), and says when it is settled: it has stood for five
seconds and is the rate now. Three things were learned by testing, each
from a wrong notice:

- Presents can come slower than the screen, steadily. With Steam's overlay
  drawing its notice (a controller connected), two 120 Hz runs had about
  half their presents for 2 to 6 seconds. A window that is covered, on
  Wayland, is shown about 20 times a second: under a lock screen in a
  headless KWin, on a 60 Hz screen, the presents came at a steady 19.7 Hz.
  So fewer presents than the report says nothing about the screen.
- A window's first presents can come faster than the screen: under Wine,
  about 80 a second for the first second on a 60 Hz screen. So only a
  settled rate counts.
- A covered window under Xwayland (Wine, Proton) is shown by a timer of
  Xwayland's, not by the screen: 62 a second and 80 a second were both
  seen on a 60 Hz screen, steadily, and with the keyboard still the
  window's. So more presents than the report is not proof that the report
  is old. It is a guess.

**What is done** (`screen_rate` in main.c). The system's report is held
against the presents' settled rate, while the window has the keyboard:

- More presents than the report, by more than a tenth and up to 400 Hz:
  the presents' rate is taken for the screen's. It is a guess, and where a
  notice comes of it, the notice says so: "The screen may not be at a
  multiple of 60 Hz", "this screen seems to refresh about 144 times a
  second ... the motion may look uneven. If it does, set the screen to a
  multiple of 60 Hz (60, 120, 180, 240)".
- The report is of an uneven rate, and the presents come steadily at 60 Hz
  or a multiple of it: the screen was set to that since the report, as the
  notice asks, and no notice is due. This can only take a notice away.
- Otherwise the report stands: fewer presents, presents within a tenth
  above it (the 62 on a 60 Hz screen), no settled rate, or the window not
  in front. The two within 1% of each other: the report's figure. No
  report at all: the presents' rate, between 48 and 400 Hz.
- The notice waits for the presents' rate to settle: two seconds into the
  picture at the earliest, as before, and twenty at the latest, where it
  goes by the report. When the window comes to the front, the rate kept is
  forgotten (`vsync_rate_forget`): what the presents did behind other
  windows does not count.
- The notice's advice is "a multiple of 60 Hz (60, 120, 180, 240)" in both
  wordings, where it was "60 Hz (or 120)": 180 and 240 Hz screens are even
  too.
- The log says which: `screen: 144.00 Hz, by its presents (the system says
  60.00 Hz)`, `screen: 60.00 Hz (the presents come at 19.73 Hz: held back,
  or the window is covered)`, or `(the system's report: the presents gave
  no settled rate)`.
- For tests: `--screen-hz HZ` is the screen's rate outright, as before;
  `--system-hz HZ` is what the system reports, with the presents measured
  as ever.

**Two rules that were wrong**, both caught before any release. The first
took the presents' rate wherever it differed from the report. The release
check for it ran while the desktop's session was locked: KWin then starts
a lock screen in the test's hidden desktop too, over the game's window,
and that version logged `screen: 19.74 Hz, by its presents (the system
says 60.00 Hz) (not a multiple of 60: uneven)` and showed the notice on a
60 Hz screen. The second took any higher rate as proof of an old report,
and stated it as a fact: a covered window under Wine gave `screen: 61.83
Hz, by its presents (the system says 60.00 Hz)` and the notice again. The
test's KWin now runs with `--no-lockscreen`, and with `--lockscreen` where
a covered window is wanted.

**A model** of the rate kept (the repository's vsync.c with made-up
presents, each 0.3 ms off its refresh):

| Presents | Settled at | On |
|---|---|---|
| every refresh, at 120, 59.94 and 144 Hz | 5.7 to 5.9 s | 120.00, 59.94, 144.00 |
| 144 Hz as the 0.5.13 run began: uneven, every 2nd for 2.2 s, uneven, then every refresh | 9.6 s | 144.00 |
| 144 Hz as the 0.5.14 run began: 3 s of every refresh, then uneven and every 2nd for 6 s | 7.5 s | 144.00, kept through the 6 s |
| 180 Hz, every 2nd for 4 s in the middle | 5.6 s | 180.00, kept |
| 120 Hz, each refresh presented with a chance of 0.6 throughout | 7.9 s | 120.01 |
| the screen raised from 60 to 144 Hz | 3.9 s after it | 144, settled 5 s later |
| the screen lowered from 144 to 120 Hz | 10.2 s after it | 120, settled 5 s later |
| 144 Hz, every 2nd for the first 8 s | 5.8 s | 72.00, until 144 at 9.4 s |

**Checked** with this build, in a headless KWin (its screen is 60 Hz) and
under gamescope's headless backend (120 and 144 Hz), sound off. "Test run"
checks at a set vertical blank (3,000, or 3,400 where covered) without
waiting for the rate to settle; the others have no test option, and the
notice comes or not by itself.

| Screen | The system made to report | The game | Logged | Notice |
|---|---|---|---|---|
| 144 Hz | 60 | Linux, test run | `144.00 Hz, by its presents (the system says 60.00 Hz) (not a multiple of 60: uneven)` | shown as a guess: "seems to refresh about 144 times a second" |
| 144 Hz | 60 | Linux, no test option | the same, 6.5 s into the run | shown by itself; `refresh-notice` says `144 Hz` |
| 144 Hz | its own (143.79) | Linux, test run | `143.79 Hz (not a multiple of 60: uneven)` | shown, in the plain wording |
| 120 Hz | 60 | Linux, test run | `120.00 Hz, by its presents (the system says 60.00 Hz)` | none |
| 60 Hz | 144 | Linux, test run | `59.94 Hz, by its presents (the system says 144.00 Hz)` | none |
| 60 Hz | 144 | Linux, no test option | `60.00 Hz, by its presents (...)`, 6.5 s into the run | none, and no `refresh-notice` file |
| 60 Hz | 144 | Windows under Wine, no test option | `59.91 Hz, by its presents (...)`, 7.1 s into the run | none |
| 60 Hz | 144 | Windows under Wine, test run, twice | once `59.97 Hz, by its presents (...)`; once the rate had not settled by then: `144.00 Hz (the system's report: the presents gave no settled rate)` | none; shown, by the report |
| 60 Hz, the window covered | its own (60) | Linux, no test option | at the twenty seconds: `60.00 Hz (the system's report: ...)`; the presents came at 19.6 Hz, and the window had not the keyboard | none |
| 60 Hz, the window covered | 144 | Linux, no test option | at the twenty seconds: `144.00 Hz (the system's report: ...) (not a multiple of 60: uneven)` | shown, by the report, as before this section |
| 60 Hz, the window covered | its own (60) | Windows under Wine, no test option | at the twenty seconds: `60.00 Hz (the system's report: ...)`; the presents came near 80 Hz at times and did not settle | none |
| 60 Hz, the window covered | its own (60) | Windows under Wine, test run | `60.00 Hz (the system's report: ...)` | none |
| 60 Hz, the window covered | 50 | Windows under Wine, test run | `61.92 Hz, by its presents (the system says 50.00 Hz) (not a multiple of 60: uneven)` | shown as a guess, wrongly: the first of the cases below |

The notice in both wordings, and the Fullscreen row's note with the longer
one, were looked at in the window's own pictures: the text fits.

**What stays wrong.**

- A covered window under Xwayland whose timer's rate settles at more than
  a tenth above the report, with the keyboard still the window's: the
  notice is shown, as a guess, where none may be due. It was made to
  happen with the report set to 50 (62 seen). With a right report of 60,
  the 62 is within the tenth, and the 80 did not settle in the runs made;
  it could.
- Presents held back from the first one on for longer than the five
  seconds, with a report that is too low as well (a 144 Hz screen showing
  every other present, reported as 60): the guess names 72.
- A report that is too high for a screen now at another uneven rate (144
  reported, 90 now): the notice names 144.
- A report that is too low while the window is behind another from the
  start: no notice in that run.
- A frame limiter holding the presents at a steady 60 or 120 on a 144 Hz
  screen: no notice.

**Not covered.** A real change of a real screen's rate while the launcher
is open, under Proton, with this build (the case it is for: the hidden
desktops cannot change their rate, so the wrong report was put in by
`--system-hz`). Windows itself. A Steam Deck. A screen with G-Sync or
FreeSync, where the presents may not come at one steady rate. Where the
old report is kept, so that it could be put right at its source.

## 62. The notice's rate: fewer presents count too; section 61 put right (2026-11-17)

**What was seen.** A 0.5.15 game under Proton on a screen at 50 Hz, which
was then set to 59.94 and to 120 Hz as the game ran. The pacing followed
each: 500, 599 to 600 and 1,200 presents to 600 frames, and the game
closed in 0.12 s. But its log has `screen: 60.00 Hz (the presents come at
49.96 Hz: held back, or the window is covered)`, and no notice came, on a
screen where one frame in six cannot be shown. Section 61's rule let fewer
presents than the report say nothing.

**Section 61 put right.** It said the report under Proton looked to be the
rate the screen had when the launcher started. This run is not that: the
screen was at 50 Hz from 21:27:13, Steam started the launcher at 21:27:23,
and the game was still told 60.00. Three runs on this PC now: told 120.00
at 120 Hz, 60.00 at 120 Hz, 60.00 at 50 Hz. Xwayland gives the screen's
rate rightly (`xrandr` on the same desktop: 119.98 at 120 Hz), so the wrong
figure is Proton's or Wine's. Why it is 60 was not found.

**What is done.** The rule is one function, `settings_refresh_choose(sys,
seen, &own)` in settings.c, so that a table can test it. main.c's
screen_rate gives it the system's report and the presents' rate, the
latter only once it is settled and only while the window is the active one
(section 61). In this order:

- The two within 1% of each other: the report's figure.
- The presents' rate in one of two ranges, 60.6 to 62.7 or 76 to 83.6 Hz:
  the report. A window that is covered under Xwayland is shown by a timer,
  not by the screen: 16 ms or 12 ms a present, less what the presents
  take. 61.0 to 62.2 and 79.2 to 82.5 a second were measured. No screen
  runs at these rates.
- No report: the presents' rate, between 48 and 400 Hz.
- A tenth or more above the report, to 400 Hz: the presents' rate.
- Below a report of an uneven rate, and at 60 Hz or a multiple of it: the
  presents' rate, and no notice (the screen was set to it since).
- A tenth or more below the report, 48 Hz or more, and not within 5% of a
  half, a third or a quarter of it: the presents' rate. This is new. The
  tenth keeps out a frame limit set a few frames under the screen's rate
  (117 on a 120 Hz screen); the fractions keep out an overlay that shows
  every other present, and a covered window on Wayland (a third or a half).
- Anything else: the report.

Where the presents' rate is taken, it is a guess, and the notice says it
as one (section 61's wording: "this screen seems to refresh about 50 times
a second").

**Does the game know when its window is not the active one?** It reads it
(`SDL_WINDOW_INPUT_FOCUS`), and the presents' rate counts only then. Tested
in a headless KWin with the game started first and covered afterwards, the
game logging its window's state and its presents' rate every two seconds
(a test build):

| Covered by | The Windows game under Wine | The Linux game |
|---|---|---|
| another window opening on top and taking the keyboard | not active 0.1 s later; its presents stayed at 60 | not active at once |
| the screen locking as the game ran | not active 0.4 s later; its presents went to about 62 | not active at once, and "occluded" |
| a change to another virtual desktop, an empty one | still "active", 3 runs of 3; its presents went to about 62 | not active at once, and "occluded" a second later |

So the game is told whenever something else takes the keyboard. It is not
told, under Wine, when it is hidden and nothing else takes the keyboard:
the empty virtual desktop of a bare KWin, and a lock screen that was up
before the game started (section 61's two wrong notices). On a desktop
with a shell, an empty virtual desktop still has the shell's own window to
take the keyboard; that was not tried. The notice is decided once, in the
game's first twenty seconds, so this takes a game hidden so in that time:
the maintainer's judgment is that hardly anyone does that, and it is left.

**Checked** with this build.

A table test of the function (the report and the presents' rate in, the
rate and "a guess or not" out): 45 cases by name, among them the
maintainer's two runs, the frame limits and the rates that failed below;
and 448 more, every rate a hidden window or an overlay was seen or can be
reckoned to give, against 14 reports. None is taken for the screen's rate.

Screens that run at a rate (gamescope's headless backend; a headless KWin
for 60 Hz), the game left to itself:

| Screen | Reported as | Logged | Notice |
|---|---|---|---|
| 50 Hz | 60 | `50.00 Hz, by its presents (the system says 60.00 Hz)` | shown, as a guess |
| 75 Hz | 120 | `75.00 Hz, by its presents (...)` | shown, as a guess |
| 100 Hz | 120 | `100.00 Hz, by its presents (...)` | shown, as a guess |
| 144 Hz | 60 | `144.00 Hz, by its presents (...)` | shown, as a guess |
| 144 Hz | its own (143.79) | `143.79 Hz` | shown, in the plain wording |
| 120 Hz | 60 | `120.00 Hz, by its presents (...)` | none |
| 60 Hz | 144, Linux and Windows under Wine | `60.00 Hz, by its presents (...)` | none |
| 90 Hz | 180 | `180.00 Hz (the presents come at 90.00 Hz, which is not taken for the screen's rate)` | none: a half |
| 117 Hz | 120 | `120.00 Hz (the presents come at 117.00 Hz, ...)` | none: within a tenth |

The game covered three seconds in, before it decides, and kept so past
the twenty seconds, the report made 120 (so that any notice is a wrong
one): another window on top, the screen locked, another virtual desktop;
the Windows game under Wine and the Linux game. Six of six decided by the
report, with no notice.

The window hidden before the game starts (a lock screen already up: the
case the game is not told of under Wine), the report made its own 60, 50,
120, 144, 180 and 240:

| Game | Runs | Wrong notices |
|---|---|---|
| Linux | 6 | none (presents at 19.6 Hz, the window not active) |
| Windows under Wine, the ranges as first set (to 82.0) | 6 | 1: 82.48 Hz against a report of 180 |
| the same, the ranges widened (to 83.6) | 12 | 1: 103.83 Hz against a report of 120, a rate seen once |
| the same, this build | 6 | none (62 and 82.5 Hz, both kept out) |

**What stays wrong.**

- A window hidden under Wine with nothing else taking the keyboard, in
  the game's first twenty seconds, whose timer's rate settles outside the
  two ranges (104 Hz was seen once): the notice is shown, as a guess,
  where none may be due.
- A screen that really is at half, a third or a quarter of the reported
  rate (90 Hz reported as 180): no notice.
- A screen within a tenth of the reported rate but uneven (55 Hz reported
  as 60): no notice.
- Presents held back from the first one on for longer than the five
  seconds, with a report that is too low as well: the guess names the
  held-back rate.
- A frame limiter holding the presents at a steady 60 or 120 on a 144 Hz
  screen: no notice.

**Not covered.** Windows itself. A Steam Deck. A screen with G-Sync or
FreeSync. An empty virtual desktop on a desktop with a shell. Why Proton
reports 60.

## 63. The refresh's period, taken again when the phase keeps starting again (2026-11-17)

**What was seen.** A 0.5.16 game under Proton on a screen at 50 Hz, set to
60, 100 and 120 Hz as it ran. The kernel's display log gives the times: 50
Hz from five seconds before the game started, 60 Hz at 83.6 s, 100 Hz at
147.0 s, 120 Hz at 198.6 s. At 60 and at 120 Hz the game's log has the
refresh steady within two seconds. At 50 Hz, for all of 83 seconds, and at
100 Hz, for 52, it has "the screen's refresh is not steady, or not
measured yet".

Two things go by a steady refresh. Frame pacing's Game clock (section 59)
showed "the newest frame at each present" all that time, which is what the
game did before section 58. And section 62's notice had no settled rate to
go by: `screen: 60.00 Hz (the system's report: the presents gave no
settled rate)`, and nothing was shown on the 50 Hz screen. The run before
it, 0.5.15 on the same screen at 50 Hz, was steady in 2.5 seconds, and
vsync.c is the same in both.

**Why.** The filter (section 58) takes its period from the first 16
intervals. It takes it again when 16 takes in a row do not fit its grid
(VS_SEED), or when 16 in a row come two refreshes or more on (VS_MULTI).
Fewer takes that do not fit (each counts 2, each that fits takes 1 off: at
VS_BAD, 6, so three in a row) only start the phase again. But with a
period that is not the screen's, takes
that fit and takes that do not can take turns:

- A period of 12.5 ms (80 a second: about what a window's first presents
  come at under Wine, before it is shown) and presents every 20 ms: every
  other take is within a quarter of a period of the grid.
- A period of 16.7 ms and presents every 10 ms (the screen set from 60 to
  100 Hz): two do not fit, the third does.

Then neither count in a row is ever reached. The phase starts again every
quarter of a second, no run grows long enough to be steady, and the wrong
period is kept for good. Which first period the filter gets is a matter of
the window's first presents: the 0.5.15 run's happened to be 50 Hz's.

A model (the repository's vsync.c, the presents made up, each 0.3 ms off
its refresh) with the filter as it was:

| The period held | The screen then | The refresh |
|---|---|---|
| 80 Hz's, from 0.4 s of presents | 50, 60 or 120 Hz | never steady |
| 104 Hz's, the same way | 60 Hz | never steady |
| 80, 62 or 104 Hz's | six other pairs with 50, 100, 120, 144 Hz | steady 0.8 to 6.0 s after the screen took its rate |
| a screen's, changed 60 to 100, 60 to 90, 90 to 60, 240 to 144 Hz | | never steady again |
| a screen's, changed in 14 other ways between 50 and 240 Hz | | steady again 0.8 to 7.6 s after the change |

**What is done.** The filter counts how often the phase has started again
since the refresh was last steady (`vs.restarts`). At the third
(VS_RESEED) it takes the period again from the latest 16 intervals, as
after 16 misfits in a row. The count is cleared whenever the refresh is
steady. So a phase that starts again now and then on a screen whose period
is right costs nothing new (on KWin's virtual screen the phase jumps by
about half a refresh now and then: section 59): there the refresh is
steady again after 8 takes that fit, before a third restart.

**Checked.**

The model with this filter: the ten first periods (80, 62 and 104 Hz's,
then a screen at 50 to 144 Hz) are steady 0.6 to 1.1 s after the screen
took its rate, and the 18 changes of rate 0.6 to 1.4 s after the change.
None is left unsteady.

Section 58's and section 59's models (28 cases each: jitter, late
presents, spikes, slow and fast screens, a change of rate) give the same
output as before, to the character, in 27 cases each. The one in each that
changes the screen's rate differs. Section 58's (60 to 120 Hz) loses less
in the change: no frame never shown where there was one, 41 takes
repeating a frame where there were 91. Section 59's (60 to 144 Hz) shows
every frame for as many presents as before; of its 18,000 frames 11,941
were moved a refresh later where 11,857 were. Section 61's model of the
rate kept settles as before or sooner.

The real game, with `--refresh-first HZ` (for tests: the filter starts
holding HZ's period, as after presents that were not the screen's, or
after a change of the screen's rate), left to itself for 20 s on screens
that run at a rate (gamescope's headless backend; a headless KWin for 60
Hz). "Without" is the same build with VS_RESEED out of reach:

| Screen | First taken for | Without the new rule | With it |
|---|---|---|---|
| 50 Hz, reported as 60 | 80 Hz | never steady; no notice | steady 2.8 s into the run; `screen: 50.00 Hz, by its presents (the system says 60.00 Hz)`, and the notice |
| 100 Hz | 60 Hz | never steady | steady at 2.8 s |
| 60 Hz | 90 Hz | never steady | steady at 3.0 s |
| 60 Hz, the Linux game and the Windows game under Wine | 80 Hz | | steady at 2.9 s and at 1.7 s |
| 120 Hz | 80 Hz | | steady at 2.8 s |
| 144 Hz | 240 Hz | | steady at 2.9 s |
| 90 Hz | 60 Hz | | steady at 2.9 s |

(The times are those of the log's first `pace:` line, about 2.8 s into a
run. In the four runs whose logs were kept, the log's lines on the frames
drawn have the screen's rate from 1.5 s on.)

The release's own games (0.5.17, built by its builder from its zip): the
eight cases again, 8 of 8, steady by 3.1 s (1.9 s under Wine), and section
62's ten real-rate cases, 10 of 10.

The hidden-window cases again, since a refresh that is steady sooner gives
the notice's rule (sections 61 and 62) a settled rate sooner:

- The window hidden before the game starts (a lock screen), the Linux
  game, six reports: 6 of 6, the presents at 19.3 to 19.8 Hz and never
  taken.
- The game covered three seconds in (another window, the lock screen, the
  other virtual desktop), the Windows game under Wine and the Linux game:
  6 of 6, no notice.
- The window hidden before the game starts, the Windows game under Wine,
  six reports twice: 11 of 12 as before (the presents at about 62 Hz, not
  taken). In the twelfth, with 50 Hz reported, the hidden window's
  presents came at about 59.5 a second for its first twelve seconds, and
  at 62 after that as in the other runs. Section 61's rule for an uneven
  report and presents at an even rate (the screen was set to an even rate
  since the report) took that: `screen: 59.43 Hz, by its presents (the
  system says 50.00 Hz)`, and no notice where the test wanted one.

That twelfth run is not this section's doing. Presents like its own (62 Hz
for 0.4 s, then 59.5 Hz) give 59.56 Hz, settled 5.9 s in, with the filter
as released and with this one, the same to the digit; the first period was
near enough to be kept either way. It is the hole section 62 left open, a
window hidden with the game not told, with a second way to go wrong: the
hidden window's timer under Wine is known by its rates (60.6 to 62.7, 76
to 83.6 Hz), and at 59.5 Hz it passes for a 60 Hz screen. Where the system
rightly reports an uneven rate, the notice is then left out. Nothing was
changed for it.

**Not covered.** Presents that are uneven in themselves and fit no grid:
there the refresh stays unsteady as before, with the period now taken
again at every third restart. A hidden window's presents near 59.5 Hz
under Wine (above). Windows itself. At the release, also a real screen
under Proton with this build, and a real change of a screen's rate as it
runs (the hidden desktops cannot change theirs, so the state after a
change was put in by `--refresh-first`): the run below has both.

**On a real screen, after the release (2026-11-18).** 0.5.17 under Proton
on the screen of the run that showed the fault. The kernel's display log
gives the times again: 50 Hz from a minute before the game started, 60 Hz
(the 59.94 Hz mode) 55.1 s into the run, 100 Hz at 82.6 s, 120 Hz at
108.9 s.

| The screen | 0.5.16's run | 0.5.17's run |
|---|---|---|
| 50 Hz at the start | never steady in 83 s; no notice | steady at 49.89 Hz in the log's line 2.7 s in (not yet in those at 1.7 and 2.2 s); `screen: 49.92 Hz, by its presents (the system says 60.00 Hz) (not a multiple of 60: uneven)` and the notice at 7.4 s |
| set to 60 Hz | steady within two seconds | set with the menu open; 59.941 Hz in the next line on the frames drawn |
| set to 100 Hz | never steady in 52 s | not steady in the `pace:` line 0.4 s after the change; 99.998 Hz in the line on the frames drawn 2.1 s after it |
| set to 120 Hz | steady within two seconds | not steady in the `pace:` line 0.5 s after the change; 120.097 Hz in the next one, 2.5 s after it |

The log's lines come at set times (a `pace:` line when the pacing changes,
but not more than one in two seconds; the line on the frames drawn every
600 vertical blanks), so the times after a change are upper bounds. At 100 Hz each 600 frames went to
1,000 presents and none was never shown. The game closed 0.2 s after Quit.

## 64. The light that makes stage objects see-through (2026-11-23)

**What was reported** (issue 2). Stage objects do not turn transparent when
they come into the line of sight, like the pillars on Sarah's stage, and
block the view of the fight.

**What the game does.** `stage_task_exec` draws a stage's third object
list, the objects near the ring, with `obj_draw_translucent` during a
fight: the models go to the translucent list with the blend of source
alpha and the polygons' Use Alpha bit turned on by the link command
(`model_draw_all_as_translucent`). Their materials are solid, so by
themselves they would look as before. Around those draws the game turns on
its "extra light" (`extra_light_spot_set`): light 14, a spot light at the
camera's eye that looks at the camera's target, with the diffuse mode 3
and the routing 12. The game's own debug menu names the values:
SPECIAL_EFFECT, and ALPHADIFF_SUB among sixteen routings (base or offset
colour, alpha, fog; added or subtracted). So the light's amount is taken
from the alpha of what it falls on: what stands between the camera and the
fighters is seen through.

**Why it was not drawn.** Two things in the view (`runtime/machine/view.c`,
which both renderers draw from):

- A light set's header (`elan_light_masks_queue`: the mask of the lights
  that are on, the ambient colour) put every light out, on the reading
  that the lights follow it. `light_set_send` sends them so. The extra
  light is sent the other way round, the light and then a header with its
  bit, so it was never on. And that header put the set's own lights out
  too: the third object list, and whatever was drawn after it until the
  next light set, were lit by the ambient colour alone. The cage stage's
  signs were black rectangles, the scaffolding of another stage black.
- A light's routing and diffuse mode were not read: every light added to
  the colour.

**What is done.** The header no longer puts lights out: a light stays as it
was sent, and the mask says which are on (it was already applied when a
strip is linked). A light keeps its diffuse mode, its routing and its
cone. A light routed to the alpha (4 and 5 add, 12 and 13 subtract) leaves
the colour alone and changes what is left of the vertex's alpha: the
light's colour, times the fall with distance, times the cone; for
SPECIAL_EFFECT (and for the two routings named ALPHAATTEN) without the
angle between the surface and the light, which would leave the far side
of an object solid. The cone is read as `elan_light_pack_spot` packs it,
for both of the ELAN's revisions: two half floats k and b, the amount
k x + b held to 0..1, x the cosine between the light's direction and the
way to the vertex (k positive) or 1 less that cosine (k negative).

On Sarah's stage the light is sent as: 5 units behind the eye and 0.21
up, looking ahead and a little up; full up to 8.41 units from itself (3.4
in front of the eye) and gone at 8.81; full within 7.6 degrees of its axis
and gone beyond 19.2.

**Checked** in the headless game (the software picture), a two-player
match forced onto a stage as the game loads it (`reg r4` at `stage_load`):

- Sarah's stage (stage 0), both fighters dodging towards the camera: once
  the camera stands outside the ring's colonnade, its pillars and beam
  cover the fighters without the change and are seen through with it (the
  parts off the light's axis stay faintly). 66 of 193 pictures, 15
  vertical blanks apart, differ from the same build with the alpha left
  alone, in up to 122,000 of a picture's 307,200 pixels.
- The same vertical blank from a snapshot in the windowed game: the GPU
  renderer's picture shows the same as the software one.
- One picture of a fight on each of the sixteen stages: nine differ from
  0.5.17's, in the objects near the ring, now lit (the cage's signs can be
  read); seven are the same to the pixel.
- With the lights counted on nine stages: every vertex drawn while light
  14 is on is in the translucent list and takes the lit path (none with
  colours of its own, none unlit).
- The attract sequence, 125 pictures: 83 the same to the pixel as
  0.5.17's. Of the 42 others the two that differ most were looked at:
  Sarah's stage behind the ranking screens, its colonnade now lit.

**What the game's code settles, and what it does not.** The cone and the
fall with distance are not guesses: they are the two pairs of numbers
`elan_light_pack_spot` computes, read back the way it packs them. That
SPECIAL_EFFECT takes no account of the surface's angle is settled by the
game's other use of the same kind of light: `extra_light_parallel_set`,
a parallel light with SPECIAL_EFFECT and ALPHADIFF_SUB whose direction is
always (1, 0, 0), sideways in view space, and whose colour is a fade
amount. The lens flare's pieces, which face the camera, are drawn under it
(`fade * 0.8`, `fade * 0.05`), and so are the round shapes of the
hair's collision display. With the surface's angle counted, a piece facing
the camera would get nothing from a light from the side and the amount
would do nothing; a ball would fade on one side only. So the amount is the
colour as it is, and for the spot times its fall and its cone.

**Held against Flycast's reading.** The ELAN has no public documentation
that was found; an emulator's reading of it is public. Flycast's NAOMI 2
light record and lighting shader were read as a reference (2026-11-23),
and nothing was taken from them. They agree with the above on each point:
a light whose routing has the alpha bit puts its amount into the alpha and
not the colour; the subtract bit turns the amount round; the single-sided
and double-sided modes alone count the surface's angle, so SPECIAL_EFFECT
does not; the fall with distance and the cone are the two pairs of half
floats, the cone on 1 less the cosine. Two things the game's code left
open are taken as Flycast has them: the amount is the red part of the
light's colour (the game sets all three alike), and it is added to the
material's alpha or taken from it, the result held to 0..1. The first
version here scaled the alpha instead; with the alpha of 1 these objects
have it is the same picture (193 compared: two differ, in one pixel each
by a rounding step). Flycast does not handle the ALPHAATTEN and fog
routings; here ALPHAATTEN is taken without the surface's angle, and no
light of the game's uses it.

The chip's maker has published the driver source of the generation
before (PowerVR Series 1, 1996-97, github.com/powervr-graphics), which was
looked at for how its authors did such things. There a light never touches
transparency: opacity is a value of the material alone, held to 0..1, and
the lighting code does not name it. So it has no rule for this light; its
habit of summing and then holding to the range is the one followed here.

**Not covered.** A comparison with the real board. An ordinary spot
light's cone: lights that go to the colour are still lit as point lights,
as before (section 89 gives them their cone). The fog routings. The translucent list's modifier volumes are
still not drawn by the GPU renderer (section 20): that is another thing.

## 65. Wide screens: a reflection goes on past the game's sides (2026-11-23)

**What was reported.** With a wide picture (section 39) the water's
reflection on the Shrine stage is right in the middle of the screen and
smears from the 4:3 picture's edges outwards.

**Why.** A stage's reflection is a picture of its own: the game draws the
mirrored scene into a texture every frame (`stage_screen_render` in
`stage_screen.c`; on the Shrine a pass of 512 x 256) and draws the water
with that texture, each point looking it up by where it is on the screen.
Section 39 widened the pass to the screen only: a pass into a texture was
left as the game makes it, for its own 640-wide view. Past the game's
sides the water's lookup ran off the texture's edge, and the edge's last
column was stretched over the rest (the right side) or the edge was dark
(the left).

**What is done** (`view.c`). On a wide screen a pass into a texture is
drawn wider too, by the wide screen's share of its own width (a 512-wide
pass at 21:9: 202 columns more on each side), with the scene moved right
as the screen's is (`scene_shift`, which now takes the pass's own width).
The game's part of it goes into texture memory exactly as before, so the
machine holds what it always held. The whole wide picture is kept beside
it (`wide_tex`), and it is used only for the screen pass it was drawn for.
A polygon of that screen pass whose texture is the pass's target is drawn
with the wide picture instead (`wide_tex_use`): a texture of twice the
width, the game's texture as decoded in its middle half (so the middle of
the screen is what it was), the columns drawn beyond on each side of it,
and the outermost column on to the ends; the polygon's u becomes u / 2 +
1/4. It is a slot of the texture cache of its own, so both renderers take
it as any texture. At 4:3 nothing of this runs.

**Checked** (the windowed game from snapshots, `--shot`, a headless
desktop; the stage chosen as the game chooses it, by `match_stage` at the
character select, so that its reflections are set up):

- The Shrine's intro over its pond at 21:9, GPU renderer: the bridge, the
  rocks and the sky are mirrored in the middle as before; left of the 4:3
  picture the walkway's pillars and right of it the ring's fence are now
  mirrored too, where there was a dark edge and a smear. The software
  renderer's picture of the same moment differs from the GPU's by a mean
  of 1.1 (of 765).
- At 4:3 the pictures are the same to the pixel as before the change (two
  moments).
- Inside the 4:3 part of the 21:9 picture, 892 of 307,200 pixels differ
  from before, 59 of them by more than 8 levels: the water close to the
  4:3 edges, whose waves took the lookup past the edge before.
- The harbour and the cave, the two stages with waves on their water,
  intros at 21:9: boats and buildings are mirrored across the whole width.
- The machine's time a frame in a fight on the Shrine, this PC: 4.4 ms
  before and 6.1 ms after at 21:9 (the wider pass is drawn by the CPU);
  4.5 and 4.2 ms at 4:3.
- The maintainer played the build on the Shrine with a wide picture: it
  looks right.

**Not covered.** The game still leaves out what lies wholly outside its
own 4:3 view (`obj_sphere_visible`, the side planes of
`stage_far_objs_draw` and `stage_obj_visible`), in the reflection as on
the screen: an object can still be missing at a side until it reaches the
4:3 picture. A stage's screens that are not water (the monitors of the
stage with the tunnel) were not looked at; they take the same path, and
show the middle half of their wide picture. A pass wider than 640 is not
widened (the buffers are 1,280 wide). 16:9 was looked at in one picture of
the Shrine (the reflection goes on past both 4:3 edges); 16:10 was run and
not looked at.

## 66. Every controller SDL reports is logged (2026-11-23)

**Why** (issue 4). A player's Xbox Elite controller on the Xbox Wireless
Adapter was taken by neither the launcher nor the game on Windows 11; it
worked after the next update, in which nothing about controllers had
changed. The logs could not say what had happened. The game logged a
controller only once SDL had it as a gamepad (`gamepad added`), so a
controller SDL saw but had no button layout for, and one SDL did not see at
all, looked the same: no line. The launcher logged no controller at all.

**What is done** (`runtime/ui/ui.c`, which both programs share).
`ui_pad_event_hidden`, which both pass every event through, writes a line
when SDL reports a joystick and when one goes: `controller seen: id 3
"Xbox Series X Controller" 045e:0b12, a gamepad: xboxone, guid ..., path
...`, or `NOT usable: SDL has no gamepad layout for it` in place of the
kind, and `controller gone: id 3`. One line as a controller comes, one as
it goes; nothing is written for buttons, sticks or frames.

**Checked.** The game's test pad (`--keys pad:`) gives the line in the
game's log. An Xbox Series controller on an Xbox Wireless Adapter (Linux,
the xone driver) gives it in the launcher's log and the game's, and the
maintainer played with it in both. The bundled Windows SDL has the XInput,
RawInput, HIDAPI, DirectInput and Windows.Gaming.Input back ends.

**Not covered.** The player's own case was not reproduced: no Elite
controller here, and not on Windows. What made it fail is not known.

## 67. Lighting on the GPU, as an option (2026-11-24)

**Why** (issue 3). On a laptop with a two-core processor (a 7th-generation
i3, Intel HD Graphics 620) the game ran in slow motion: its log has the
machine's thread at 22.6 to 23.0 ms a frame in a fight, where 16.7 is all
there is, while the screen still got its 60 presents a second. So the
graphics chip had time to spare and the processor had none.

Where the machine's time goes was measured (temporary timers, removed): on
the development PC a fight frame is 4.5 ms, of which the picture's work is
2.2 (reading the lists 0.3, transform and lighting 1.4 on eight threads,
the hand-over 0.5), the sound chip 0.75 and the game's code the rest. On
one thread the transform and lighting is 2.9 ms. Inside it (`strip_job`:
about 85,000 vertices a frame, three in four lit, by three lights each on
average) the colours and lights take 53% of the time, putting triangles
together 28% and the transform 18%.

**What is done.** A setting, Lighting (settings.txt `lighting = cpu | gpu`,
the Picture tab of the game's menu and of the launcher's Settings; CPU by
default), moves the lights of the GPU renderer's picture to its vertex
shader. The software renderer's are always the processor's.

- `view.c` (`view_host_gpu_lighting`): for a pass the GPU draws, a strip
  is lit by the GPU when every vertex of it would take the lights: no
  vertex with colours of its own, no bump map, not a material that only
  takes its base colour. For such a strip `strip_job` does not run the
  lights. Each vertex's eight colour floats carry the lights' inputs
  instead: the normal (view space, unit length) and the point it is lit at.
  That keeps the rest as it was: a triangle of one colour copies its last
  vertex's "colours", and the near plane's cut goes between two vertices'.
  A triangle's record says which strip it is of (`dtri.gl`).
- The hand-over (`view_gpu_frame`) gains two tables: a record for each
  strip (the material's base and specular colours, the specular power,
  the light set's number, whether the material takes no lights) and the
  light sets (the ambient colour and fifteen lights each). A vertex gains
  its normal, the point it is lit at and its strip's number.
- `scene.vert` works the colours out by the same rule as `strip_job`,
  the light that goes to the alpha (section 64) included, and takes whole
  steps of 255 as the bytes the processor's path sends. The two tables are
  storage buffers of the vertex stage (`gpu_render.c`).

Environment-map coordinates are still the processor's (they need only the
normal). Bump-mapped strips and strips with colours of their own stay on
the processor.

**Checked** (the development PC; the windowed game from snapshots, in a
headless desktop):

- 88% of the vertices of a fight are lit by the GPU.
- The picture with Lighting GPU against Lighting CPU, GPU renderer:
  Sarah's stage at seven moments (4:3 and 16:9, the fading colonnade of
  section 64 among them), the same to the pixel; the Shrine's intro at
  21:9, three moments: the same, or 27 and 203 pixels differing by one
  step of 255.
- The software renderer's picture is the same to the pixel whatever the
  setting says.
- The machine's time a frame in a fight (16:9, no sound chip, as fast as
  it goes): 3.8 ms with the processor's lights and 3.4 with the GPU's at
  full speed; 4.85 and 3.6 on two cores with their second threads; 21.7
  and 14.5 on an imitation of the laptop (those four threads held to 40%
  of one core in all, which gave the laptop's 22 ms; it weighs work shared
  between threads too heavily, so the laptop's own gain will be less).
- The launcher's Picture page with its tenth row (`--shot picture`): all
  ten show.

**Not covered.** The laptop itself, or any Intel graphics chip: the
vertex shader now reads two storage buffers, which not every old driver
may like, and that is why the setting is CPU by default. Windows itself
(the Windows build was compiled, not run with the setting). A Steam Deck.
The game's menu's Picture tab was not looked at with its new row.

## 68. Wide screens: the stage's far objects past the game's sides (2026-11-24)

**The fault** (the maintainer's report). On a wide picture, scenery at the
sides came and went as the camera turned: scaffolding, towers, pieces of
water. Section 39 had it under "not checked".

**The cause.** A wide picture (section 39) shows what lies left and right
of the game's 4:3 view, but the game leaves out of its lists what that
view does not hold. It has three tests:

- `obj_sphere_visible`: a model's sphere against the target rectangle, for
  `obj_draw_culled` (a stage's main list, its placed objects, the models of
  a reflection).
- The view's four side planes at `view_side_planes`, which
  `proj_from_vfov` and `proj_from_hfov` make from the field of view and
  the 4:3 aspect, read by two routines only (the image has four
  references to the planes: the two that write them and these):
  `stage_far_objs_draw`, for a stage's far list (objects farther from the
  eye than the camera's target; all of them in replays), and
  `stage_obj_visible`, for the water patches of stages 19, 20 and 21.

A survey with temporary counters (removed) ran a scripted two-player
match on every stage in the headless game, from the stage's intro for
about 2,600 vertical blanks with the fighters dodging and walking apart,
once with the game's tests and once with them for 21:9:

- The rectangle test turned away nothing at the sides that a 21:9 picture
  shows, on any stage: what it turns away is behind the camera (and, on
  stage 20, five spheres far past the right side). The same over the
  attract mode (14,000 vertical blanks) and over matches won by knock-outs,
  with their replays and victory poses, on eleven stages.
- The far list drew more for 21:9 on stages 3 (31,275 objects against
  43,918), 23 (15,080, 18,451), 22 (1,767, 2,948), 16 (14,093, 14,988), 17
  (6,729, 7,311) and 4 (3,740, 3,773), and in the knock-out matches on
  every stage tried.
- The water patches: stage 21 5,300 against 7,258, stage 19 9,825 against
  10,588, stage 20 8,533 against 8,216 (fewer: see below).

So the whole fault is the planes.

**What is done.** `runtime/machine/hle_cull.c`: a hook on `proj_from_vfov`
and `proj_from_hfov` runs the routine itself and then sets the left and
right planes wider: the tangent of their angle times the width over 640.
The two tests are the game's own code still, and the top and bottom planes
are the game's. The sum is +, *, / and a square root on single floats,
which give the same bits on every PC. No event is served while the routine
runs (a few hundred instructions of sums): a snapshot taken inside it
would resume without the hook's frame and leave the planes narrow on the
PC that loads it.

Why wider planes cannot take anything out of the picture: the plane test
keeps every sphere that reaches into the view, so it keeps every sphere
that reaches into the wider view, and that view holds the 4:3 one. It is
not the other way round for a sphere whose centre is behind the camera:
the narrow planes may keep one that the wide planes turn away (stage 20's
fewer patches), but such a sphere reaches into neither view.

The rectangle test is left alone. A first try widened its rectangle too;
for a sphere whose centre is behind the camera that test, as the game
writes it, gets stricter with a wider rectangle, and it was never the
fault.

**Which width** (`game_machine_cull`, `runtime/host/main.c`
`cull_width`). The picture's own: at 4:3 and stretch that is 640, the
hook does nothing and the machine is the game's. In a two-PC game it is
1,146 (21:9, the widest picture) on both PCs whatever each shows: the
planes are in the machine's memory and the lists follow them, so two PCs
with different widths would not agree (the headless game's fingerprint
after the same fight is another with 640 than with 1,146). The log says
so. The headless game has `--cull-wide W` for tests.

**The cost.** The most on stage 21: 52 million instructions more over the
run, about 20,000 a vertical blank where a fight's takes 1.2 million (and
the virtual frame holds 3.3 million). The picture's time a frame at 21:9
did not move outside what two runs differ by.

**Checked** (the development PC; the windowed game from the headless
game's snapshots in a headless desktop, the fighters dodging so that the
camera turns):

- 21:9, release 0.5.19 against this, GPU renderer, the intro and seven
  moments of a fight: stage 3 differs in 802 to 15,376 pixels a picture,
  stage 23 in up to 11,568, stage 16 in up to 12,170, stage 22 in up to
  3,935, stage 19 in up to 2,619, stage 21 in up to 182; stages 17 and 20
  showed none at these moments. Looked at one for one: scaffolding, a
  tower, a machine, a grating and water that were missing at the sides.
  Of the 113,535 pixels that differ in the 63 pairs, none is in the middle
  640 columns.
- Stage 3 at two of those moments again: the software renderer at 21:9
  differs in 15,374 and 14,760 pixels, as the GPU renderer does (15,376
  and 14,622); 16:9 differs in none and in 1,342.
- 4:3, the same two games on stage 3 at seven moments: the same to the
  pixel.
- The game's own 640 x 480 picture with the planes for 21:9 (a 4:3
  player's in a two-PC game) against the game's own planes: the headless
  game's software picture at 12 moments of the match on each of 24 stage
  numbers, 288 pairs, all the same.
- The interpreter (`--interp --cull-wide 1146`) and the translated game
  ran the same number of instructions to the same vertical blank
  (4,972,358,516 to 9,084).
- The rollback test (`--synctest 8,60`) at 21:9 on stages 3 and 21: 33
  checks each, every replay the same.
- A two-PC fight, the Linux game hosting at 4:3 and the Windows game
  joining under Wine at 21:9: 154 fingerprints agreed on both, 18 of 18
  logged fingerprints in common.

**Not covered.** 16:10 by pictures, and 16:9 on one stage only (the same
sum with another width). A real ultrawide screen by eye. Other things that may come and go
at a wide picture's sides and are not the stage's objects (effects,
shadows): none was seen, and none was looked for one by one.

## 69. A controller and buttons for each player (2026-11-24)

**Why** (the maintainer's ask). The controls had three tabs in the
launcher's Settings: Gamepad, Keyboard 1 and Keyboard 2. Every gamepad
shared one set of buttons, the first pad connected was player 1's and the
second player 2's with no say in it, and the buttons were named as the pad
last pressed names them. Wanted: one page for the keyboard and one for
gamepads with both players on each, buttons a player can change apart
from the other's, a controller that can be given to a player, and each
player's buttons named as that player's controller names them.

**The settings** (`settings.h`, `settings.c`). `pad[2][ACT_COUNT]`: a set
of gamepad buttons for each player, the same by default.
`pad_wish[2]`: the controller each player asks for. The file has
`pad1.punch`, `pad2.punch`, `pad1.controller` and `pad2.controller`; a
`pad.punch` line (0.5.20 and before) is read as both players' and written
back as the two.

**A wish for a controller** (`ui.c`, `ui_pad_wish_of`, `ui_pad_players`).
"" is Auto. Else `VVVV:PPPP[/SERIAL][@N] NAME`: the maker's and the model's
numbers, the serial number when the pad tells one (else its place among
the connected pads of that model, left out for the first), and its name
for showing when it is not there. SDL's GUID was not used: on Linux it
differs between the same pad on a cable and on Bluetooth. The line cannot
hold a `#` (it would end the settings file's line), so the name loses it.

Who has which pad, worked out every frame from the pads there (without
Steam Input's twins, section 30):

1. A player with a wish gets that controller when it is there: the one
   with the serial number, else the Nth of the model, else the first of
   the model that is free. Player 1's wish goes first.
2. The other pads go out in the order they connected, player 1 first, to
   each player who has none yet. That is a player on Auto, and a player
   whose controller is not there: a lone pad plays, whatever was chosen.

With no wishes this is what it was: the first pad is player 1's.

**The game** (`main.c`). `read_controls` takes each player's pad from
`ui_pad_players` and reads it with that player's buttons
(`pads[2][ACT_COUNT]`). The log says whose pad is whose when that changes
("gamepads: player 1: ...; player 2: ..."). In a two-PC game this PC's
player still plays with player 1's controls.

**The pages.** The launcher's Settings have Gamepad and Keyboard. Each is
a table (`ui_bind_table`): an action a row, a cell for player 1 and one
for player 2 under their heads. Up and Down choose the row, Left and Right
the player; Set takes the next key or button for the cell, Clear empties
it, Default puts the default back; a click on a cell chooses it and sets
it. The Gamepad tab's first row is Controller: Set goes to the next
gamepad connected now that the other player was not given, and past the
last to Auto; Clear is Auto. The cell says "Auto: NAME", the controller
chosen, or "Meanwhile: NAME" when the chosen one is not connected and
another pad stands in. The table takes the page's width; what the panel on
the right said is one line under it.

Each column of the Gamepad table is drawn as its player's pad names its
buttons (`ui_pad_caps_as`: the style and the four face labels of that
pad, where the caps were always the last pad's used): an Xbox pad's
letters in one column and a DualSense's shapes in the other. The buttons
are kept by place, as before, so a button set with one pad means the same
place on another.

The game's menu has the same table on its Controls tab, to read: the keys
or the gamepads, whichever was used last, and for gamepads who has which.
Before it showed player 1's keys or the one set of buttons.

**For tests.** `ui_test_pad`: SDL's virtual gamepad with a real pad's
ids. The game's `--keys` has a second one (`pad2:`, `pad2type=`), the
launcher's pictures `--shot-pads TYPE[,TYPE]`; `--shot-keys` now gives a
key its scancode, which a controls cell takes.

**Checked** (the development PC; the launcher's pictures with no window,
the game in a headless desktop, virtual gamepads):

- The launcher's pages with an Xbox and a DualSense pad, and a Switch and
  an Xbox pad: each column in its pad's names (guard, punch and kick: X,
  Y A, B beside square, triangle cross, circle; Y, X B, A for the Switch
  pad).
- Set on player 2's Controller cell: player 2 has the first pad, player 1
  (Auto) the other, and the columns' names change places. Clear: Auto.
  A chosen pad that is not connected: "Meanwhile: ..." and the line under
  the table. A click on a cell. A key set for player 2 and one cleared for
  player 1. Saved: the file has `key2.left = F`, `pad2.controller =
  045e:0b12 ...`.
- A file with 0.5.20's `pad.guard`, `pad.punch`, `pad.kick` lines: both
  players have them, and the file written has `pad1.` and `pad2.` lines
  and no `pad.` line.
- The game with an Xbox pad connected first and a DualSense second,
  `pad1.controller` asking for the DualSense and `pad2.start =
  leftstick`: the log has "player 1: DualSense..., player 2: Xbox..."
  Start on the Xbox pad did nothing; its left stick button brought player
  2 into the game; Start on the DualSense brought player 1 in.
- The game's menu, Controls tab, with the two pads: the table, the two
  controllers' names, each column in its pad's names; with the keyboard
  used last, the keys.
- Both builds compile (Linux, Windows).

**Not covered.** Real controllers: two of different makes, two of the same
model (told apart by serial number or by place), a pad on a cable and then
on Bluetooth. Steam Input, where every pad is Steam's own model and they
are told apart by place only. The Windows launcher. The pages by hand in a
window (they were driven by scripted keys and looked at as pictures).

## 70. The launcher's and the game's windows start maximized (2026-11-24)

**Why** (the maintainer's ask). The launcher's window started at the
page's own size (960 x 600 times the desktop's scale, at least half the
screen's height): small on a handheld's screen in its desktop mode and on a
television, where nothing says it should be fullscreen (that is the game's
Fullscreen setting: on, or auto on a Steam Deck or in Big Picture, section
44). The game's window started at Auto: the largest listed size that leaves
a tenth of the screen free (section 40). A maximized window is the default
for both from now on; fullscreen stays the setting's, and the game's
Window size still names a size for whoever wants one.

**The launcher** (`launcher.c`). The window is maximized as it is shown
(`win_show`: at the start, after the game ends, and when Start the game
directly shows it after all): `SDL_MaximizeWindow` on the hidden window,
which SDL applies as it shows. It is a window still, with its title bar.

A player who takes it out of maximized (the title bar's button, a drag)
has a plain window from then on: `launcher-window.txt` beside the launcher
says "window", and the next start shows the first size as before.
Maximizing it again removes the file. Only the player's own changes
count: SDL's maximized and restored events while the window shows as a
window, not while it is hidden for the game, minimized, or fullscreen by
the setting (`win_full`). A `--shot` picture keeps the page's own size.
For tests, `--unmaximize-after S` takes the window out of maximized as the
title bar's button does.

**The game** (`settings.h`, `settings.c`, `options.c`, `main.c`). Window
size has a new value, Maximized (`window = maximized`,
`WINDOW_MAXIMIZED`), and it is the default. In the row's list it is past
the largest size, at the right end: Left from it is the largest size that
fits, and from there on the row is as it was (section 51). The window is
made maximized (not when it starts fullscreen: then it is maximized when
fullscreen ends, as any Window size waits for that), and
`window_to_setting` maximizes it when the setting changes to Maximized and
restores it to a size when the setting names one. Under Maximized the
window's size when it is taken out of maximized is Auto's. The picture
keeps its shape in the middle, as in fullscreen.

A settings file written before this has `window = auto`, the default then,
whether the player chose it or not. So the file has a layout number now
(`layout = 2`): a file without it that says auto is read as maximized, and
the next save writes the number, after which auto means Auto. A size in an
old file stays that size. Unlike the launcher's, the game's window does not
keep what the title bar's button did: the setting is what it starts with.
The log has a line when the window is maximized or taken out of it.

**Checked** (a headless desktop of 1920 x 1080; the Linux launcher, and
the Windows launcher under Wine, each run from a folder of its own):

| Case | Linux | Windows under Wine |
|---|---|---|
| No file | 1920 x 1052 pixels (the room under the title bar); no file written | the same |
| `--unmaximize-after 3` | 960 x 600, "no longer maximized (kept for the next start)", the file says window | the same |
| Started again with the file | 960 x 600, not maximized | the same |
| No file, the game's Fullscreen on | fullscreen, 1920 x 1080; no file written | the same |

The picture of the first case shows the launcher's page filling the
screen under its title bar.

The game, in the same desktop (its log's window lines; the Linux game and
the Windows game under Wine gave the same):

| Case | Result |
|---|---|
| No settings file | Window size Maximized; maximized, 1920 x 1052 pixels, the 4:3 picture in the middle |
| A file without a layout number that says `window = auto` | read as Maximized; maximized |
| `window = 720` | a 960 x 720 window |
| `layout = 2` and `window = auto` | Auto's window (1280 x 960 on Linux, 1200 x 900 under Wine, whose room is smaller) |
| `fullscreen = on` | fullscreen |
| In the menu, Window size Left twice, then Right twice | the largest size that fits, the next smaller, the largest again, then maximized again |
| `fullscreen = on`, then F11 three times | a maximized window, fullscreen, a maximized window again |

**Not covered.** A real Steam Deck in its desktop mode and in Game Mode
(there the setting makes it fullscreen as before), Windows itself, Proton,
and the launcher's window after the game ends (the same call as at the
start). A
window manager that maximizes nothing (a tiling one) leaves the window as
it places it; nothing is written then.

## 71. A slow processor: less work on the machine's thread, and Frame skip (2026-11-24)

**Why** (issue 3 again). The laptop's owner tried Lighting GPU (section 67)
on 0.5.19: no fault in the picture on Intel HD Graphics 620, and the
machine's thread went from 22.6 to 23.0 ms a frame in a fight to 19.0 to
20.4. The screen still got every frame. Full speed needs 16.7 or less, so
the processor was still the limit.

**Where the thread's time goes.** No profiler is installed on the
development PC, so the game got a temporary one (removed): a timer on the
machine thread's own CPU time that takes the instruction pointer every
half millisecond, sorted afterwards by `nm`'s symbols. (A timer on the
process's time signals the main thread, and SDL blocks SIGPROF in the
threads it makes: both had to be seen first.) The run: the game held to
two cores and their second threads (`taskset`: the laptop's shape, not its
speed), GPU renderer at 640 x 480's 16:9, Lighting GPU, sound on, the
scripted fight from vertical blank 4,800 to 9,000 as fast as it goes. The
thread took 4.00 ms of CPU time a frame and 4.05 of the wall's: it is
busy, not waiting.

| Share | What |
|---|---|
| 27.2% | the game's own code, translated (a third of it its skinning of the fighters) |
| 12.2% | the picture: transform and triangles |
| 9.5% | sound: the sound processor's driver |
| 9.3% | the C library: copying, and qsort |
| 9.1% | sound: the chip's channels and effects |
| 6.8% | the picture: reading the lists |
| 6.5% | the picture: hand-over to the GPU |
| 6.2% | the picture: the comparisons of the translucent triangles' sort |
| 4.8% | device writes and reads |
| 2.5% | looking a routine up by an address |
| 2.4%, 1.7%, 1.0% | the memory watch, the runtime between blocks, the interpreter |

**Three things done for nothing, or the long way.**

- *The table of device accesses.* VF4debug keeps every device access by
  the routine that made it, for its closing report (`hw.c`,
  `note_access`): a binary search of the routines at every access. The
  game never prints it. `hw_access_notes` is 1 by default (the headless
  program's report is as it was) and the game sets 0: only accesses no
  model answers are kept.
- *A store queue's word.* The game sends its lists through the store
  queues, eight words a block, and each word took the whole slow path of a
  store that is not into RAM (`rt_io_write`: the rollback mark, the
  code-page test, the clock put right, `io_write`). It now goes straight
  into the queue.
- *The translucent triangles' order.* `qsort` with a comparison a call.
  `order_sort` gives the same order with a radix sort: the depth as an
  unsigned number that rises as the float does (-0 as 0), three passes
  that keep the order of equals, so equal depths stay in the order of
  their numbers as the comparison had them. Numbers that do not come in
  rising, or a depth that is not a number, go to `qsort` as before.

After these the same run takes 3.39 ms of CPU time a frame. The machine is
the same: the headless game's fingerprint after a scripted match is
4285abed93237ec5 at vertical blank 9,118 after 4,687,557,790 instructions,
before and after.

*Looked at and left.* The sound processor's driver is not idle between
its jobs: it scans its tables in a loop (58 to 66% of its instructions are
in three loops of six or seven instructions, each a pass over 64 records),
so there is no wait to skip exactly, and running it for fewer instructions
a sample would change what the machine does. It stays at 128 a sample.

**Frame skip.** A setting (settings.txt `frameskip = off | half`, the
Picture tab of the game's menu and of the launcher's Settings; off by
default): with it on, the screen is drawn for every second frame the game
renders, 30 a second, while the game runs every frame as ever. The view
already had the means (`every`, for the headless program's pictures):
`view_host_every` sets it for the game window. The lists of a frame that
is not drawn are not taken, so its reading, transform and hand-over are
all saved. A pass into a texture is drawn throughout, as in a re-run frame
(section 56): its picture goes into the machine's texture memory. So the
machine is the same whatever the setting says, and each PC of a two-PC
game chooses for itself. (Section 78 changed this: a pass now goes as the
screen it is made for, and is not drawn for a frame that is left out.)

**Checked** (the development PC):

- Pictures against release 0.5.21's, the windowed game from the headless
  game's snapshots: 12 pictures at 4:3, 16:9 and 21:9 on the GPU renderer
  and at 4:3 on the software renderer, the same to the pixel. Those four
  runs' time a frame: 3.63 to 2.95 ms, 4.52 to 3.81, 5.56 to 5.23 (a
  water stage, whose reflection's pass is most of its work) and 8.68 to
  7.67 (software).
- Frame skip at the game's own pace: 300 frames drawn in 600 vertical
  blanks, none of them never shown, 600 presents; the pictures shown at
  vertical blanks 5,600 and 5,601 are the same, 5,602 and 5,603 the same,
  and the two pairs differ.
- Frame skip and the machine: the fingerprints every 500 vertical blanks
  of the same fight with it off and on, 18 of 18 the same. A two-PC
  fight, the Linux game hosting with Frame skip on and the Windows game
  joining under Wine with it off: 154 fingerprints agreed, 18 of 18 logged
  ones in common.
- The machine thread's time a frame in the fight, held to two cores and
  their second threads, as the log gives it (wall time, two runs each):

  | | Lighting CPU | Lighting GPU |
  |---|---|---|
  | release 0.5.21 | 4.44, 4.48 ms | 3.90, 3.82 ms |
  | this | 3.90, 3.91 ms | 3.07, 3.55 ms |
  | this, Frame skip | 2.77, 3.17 ms | 2.36, 2.61 ms |

  With the profiler's CPU time: 3.35 ms without Frame skip, 2.54 with.
- The row in the game's menu and in the launcher's Settings (the Picture
  tab has eleven rows now and scrolls); the file written has
  `frameskip = half`.
- Both builds compile (Linux, Windows).

**What it should mean for the laptop**, if its 20 ms scales as this PC's
does: about 17 ms without Frame skip and about 13 with it. That is an
estimate from another machine, not a measurement.

**Not covered.** The laptop itself. Windows itself. Frame skip with Frame
pacing at Screen refresh or Off (Game clock was run), and by eye on a real
screen. A setting that skips frames by itself when the game falls behind
was not built.

## 72. The controls changed in the game's menu too (2026-11-24)

**Why** (the maintainer's ask). The game's menu only showed the controls
(section 69); they were changed in the launcher's Settings. A player who
sets the game up once and turns on Start the game directly (section 29)
does not see the launcher again, so the menu has to do all of it: a
controller for each player, the buttons and the keys.

**One page for both.** The launcher's two controls tabs moved out of
`launcher.c` into `runtime/ui/controls.c` (`ctl_page`: the row and the
player's cell chosen, and whether the cell waits for its key or button;
`ctl_draw`, `ctl_move`, `ctl_side`, `ctl_set`, `ctl_clear`,
`ctl_default`, `ctl_event`, `ctl_point`). The launcher's Settings and the
game's menu both use it, so the two cannot drift apart: the table, the
Controller row, its texts and what Set, Clear and Default do are the same
code. The new file is in both build lists (`CMakeLists.txt`,
`tools/vf4build/build.py`).

**The game's menu** (`menu.c`). Its Controls tab is two tabs now, Gamepad
and Keyboard, as in the launcher. Up and Down choose the row, Left and
Right the player; Enter or A sets the cell (the next key or button
pressed is it), Delete or X empties it, Backspace or Y puts the default
back; a click on a cell sets it. On the Controller row Set goes to the
next gamepad connected. A change applies at once (`MENU_LIVE`: the
window compiles the bindings again, and who has which gamepad is worked
out every frame anyway) and is saved with the other settings as the menu
closes.

**The next key or button** (`main.c`). While a cell waits
(`menu_capturing`), the window gives every key, button and axis event to
`menu_capture` before anything else: the key that would toggle
fullscreen, close the menu or move the choice is the cell's instead (Esc
alone leaves the cell as it was). The D-pad and the left stick, which the
window reads as held rather than as events, move nothing while a cell
waits, nor afterwards until they are let go: a D-pad direction taken as a
button does not also move the row.

**A cell that waits still takes Clear and Default** (the maintainer's
report, the same day). The bar under the page offered Clear (Del) and
Default (Backspace) at all times, but once a cell waited for its key or
button they did not do it: on the Gamepad page every key but Esc was
dropped, and on the Keyboard page Del and Backspace were taken as the
cell's key (reproduced: the file had `key1.punch = Delete` and `key1.kick
= Backspace`). A cell waits after Enter or a click, which is where a
player then reaches for Del. Now, while a cell waits, Del empties it and
Backspace gives its default back on both pages, and on the Keyboard page
a pad's X and Y do the same (`ctl_event`, `wait_key`); Esc, or a pad's B
there, leaves it as it was. So Esc, Delete and Backspace cannot be a
control's key from these pages. The bar shows only those three while a
cell waits (`ctl_waiting`), and nothing on the Gamepad page worked with a
gamepad, where every button would be the cell's. The mouse chooses the
cell under it, not only its row: Clear and Default act on the cell that
is lit.

**Checked** (the development PC; the game in a headless desktop with two
test gamepads, an Xbox pad and a DualShock 4; the real DualSense that was
connected at the time was kept out of the test with SDL's ignore hint,
since a test pad of its model would have been ignored with it):

- Gamepad tab: Set on player 2's Controller cell gave player 2 the Xbox
  pad and player 1 (Auto) the other; the columns' names changed places,
  and the log has the new "gamepads: player 1: ...; player 2: ..." line
  at once. A cell set with the pad's left stick button shows it; "Press a
  button" while it waits.
- A D-pad direction taken as a button: the cell has it and the row did not
  move; the next press of the D-pad moved one row; X emptied that cell; B
  closed the menu.
- Keyboard tab: a key set for player 2, a cell of player 1 emptied with
  Delete.
- The file written as the menu closed has `pad2.controller = 045e:0b12
  Xbox Wireless Controller`, `pad2.punch = leftstick`, `key2.left = F`,
  `key1.guard = -`, and in the second run `pad1.punch = dpdown`,
  `pad1.kick = -`.
- The launcher after the move to the shared page: its controls pages were
  driven with the same keys as before (a key set for player 2, one
  emptied, a controller given, a cell's default put back, a click while a
  cell waits ignored, saved): the file has what was set.
- The waiting cell, in the game's menu: on the Gamepad tab Enter then Del
  left the cell "Not set" (it kept waiting before), Enter then Backspace
  gave `y, a` back, Enter then Esc changed nothing; on the Keyboard tab
  Enter then Del emptied the cell, a key set and then Enter and Backspace
  gave the default back. The bar read Leave it, Clear and Default while a
  cell waited. The mouse moved over player 2's cell and Del: that cell was
  emptied, player 1's kept. The launcher's pages the same (the file:
  `pad1.punch = -`, `pad1.kick = b`, `key1.throw = -`).
- Both builds compile (Linux, Windows), and the builder compiles the new
  file (128 files).
- Two real controllers, a DualSense on a cable and an Xbox Series pad on
  the wireless adapter, with no test pads and no hint (the pages driven by
  the keyboard's scripted keys; no button of theirs was pressed):
  - The launcher's Gamepad page: "Auto: DualSense Wireless Controller"
    for player 1 with the shapes, "Auto: Xbox Series X Controller" for
    player 2 with the letters. Player 2 given the DualSense, saved, and
    the page opened again: player 2 has it, player 1 (Auto) the Xbox pad,
    and the columns' names have changed places.
  - The game: both are seen ("ps5" and "xboxone"), player 1 the DualSense
    and player 2 the Xbox pad. In the menu, Set on player 2's Controller
    cell: the log has "player 1: Xbox Series X Controller; player 2:
    DualSense Wireless Controller" at once, and the file has the wish with
    the pad's serial number (the game has the pads open; the launcher's
    `--shot` does not, so its wish had none).
  - The game started again with that file: player 2 has the DualSense
    from the first frame. Delete on the cell: Auto again, and the log and
    the file say so.

**Not covered.** A button of a real controller pressed: taking one for a
cell, and each controller moving its own player in a fight (the test
gamepads did both, section 69). Two real controllers of one model. The
mouse on the game's controls pages. A two-PC game with the menu open (the
game does not pause there). Windows itself.

## 73. Ambient occlusion out of the depth buffer (SSAO), as an option (2026-11-24)

**Why.** The maintainer asked for it, as an option. The cabinet's picture
has no ambient occlusion: a fighter's foot does not darken the floor it
stands on, and a fold of cloth is as bright inside as out. SSAO (screen
space ambient occlusion) gives a little of that from the depth buffer
alone, on any GPU the game's GPU renderer runs on.

**What is done.** A setting, Ambient occlusion (settings.txt
`occlusion = off | ssao`, the Picture tab of the game's menu and of the
launcher's Settings; off by default), for the GPU renderer. A change
applies at once. The software renderer has none, and its row says so.

- Where in the frame (`gpu_render.c`). A frame was one render pass. With
  the setting on, the pass ends after the opaque and punch-through lists
  and the shadow volumes; the depth buffer, now made as a texture a shader
  can read, goes through two small passes; then a pass goes on from what
  the first left (color, depth and stencil loaded), multiplies the
  occlusion into the picture, and draws the translucent list as before. So
  translucent polygons are neither darkened nor do they darken anything.
- The game's 2D layer is not touched. The game sends its bars, numbers and
  flat screens straight to the rendering chips, in the same lists and with
  depths of their own. `view.c` now says of each batch whether it is a
  model's strips, the 3D scene (`view_gpu_draw.solid`), and the opaque and
  punch-through pipelines write that into stencil bit 3 with the bit they
  already wrote (bit 0, the polygon takes shadows). The occlusion is
  multiplied in only where bit 3 is set. A 2D pixel's depth is far from
  any scene pixel's, so it does not count as something standing near one
  either.
- The occlusion (`ssao.frag`). A pixel's place before the eye comes back
  from its depth by the scene's projection, which `view.c` now hands over
  with the frame (`view_gpu_frame.proj`: its first strip's, with the wide
  picture's shift; a frame with no strip, a 2D screen, gets no occlusion
  pass at all). The surface's normal is made from the neighbours' places,
  on each axis from the neighbour nearer in depth, so that an edge's far
  side does not tilt it. Twelve places on a spiral within 0.45 of the
  scene's units of the pixel (a fighter is about 1.7 tall), drawn smaller
  with distance and left out when it comes to under a pixel and a half:
  each one that stands over the surface counts by the cosine of its angle
  to the normal, and less the farther it is, to nothing at the radius. The
  spiral is turned by one of 16 angles by the pixel's place in a 4 x 4
  block.
- The smoothing (`ssao_blur.frag`): the mean over that 4 x 4 block, of the
  pixels within a tenth of the pixel's own distance only, so the pattern
  goes and an edge keeps its side.
- The depth buffer is a texture only while the setting is on (the targets
  are made again when it changes), so with it off a frame is drawn as
  before, pass for pass. A device that cannot read its depth buffer in a
  shader, or has no one-channel target, draws as with the setting off (the
  log says so).

Found on the way: `gpu_render_quit` released four of the renderer's six
buffers (the two of section 67's lighting on the GPU were left). It
releases all six now.

**Checked** (the development PC; the windowed games from stage snapshots
in a headless desktop, the picture shown at a vertical blank in a fight,
internal resolution 1280 x 960 unless said):

- Off: the picture is the same to the pixel as before the change.
- SSAO against off, three stages: about 100,000 of the 1,228,800 pixels
  differ, on the fighters (folds, under the arms, between the legs) and
  on the floor around their feet; the largest change is 39 to 49 steps of
  255. A band inside a health bar: no pixel differs.
- A wide picture (16:9): its middle against the 4:3 picture differs by
  no more than the two differ with the setting off (the largest
  difference the same 5 steps, the mean a hundredth of a step: the 4 x 4
  pattern falls on other pixels there). So the projection handed over is
  right with the wide picture's shift.
- Internal resolution 640 x 480, 1920 x 1440 and 2560 x 1920: the game
  runs and the pixels that change grow with the picture's area. The
  software renderer: the same picture with the setting on as off.
- The game's menu: the row set to SSAO during a fight; the file has
  `occlusion = ssao`, and the picture 200 vertical blanks later is the
  same to the pixel as with the setting on from the start.
- The launcher's row, Linux and the Windows launcher under Wine: both
  save `occlusion = ssao`.
- The Windows game under Wine: the same pictures to the pixel as the
  Linux game, off and on.
- Two GPUs: an NVIDIA RTX 5070 Ti and the Radeon 8060S (Mesa's RADV). The
  two differ with SSAO by about what they differ without (a mean of a
  hundredth of a step against half of that), and the occlusion's size is
  the same on both.

**Not covered.** What it costs: the GPU's time for the extra passes was
not measured, and no slow GPU was tried (it is off by default, and the
laptop of issue 3 should leave it off). The picture in motion on a real
screen, by eye: only still pictures were looked at. Intel graphics, a
Steam Deck, Windows itself. The occlusion darkens the whole color of a
pixel, the lights' direct part too, as SSAO does; its radius and strength
are fixed (`AO_RADIUS`, `AO_STRENGTH`), not settings.

## 74. Ray traced light, step 1: Vulkan (2026-11-24)

**Why.** The maintainer asked for a path tracing setting, with reflective
glass, water and metal, and decided on two renderers for it: one on Vulkan
and one on DirectX 12, each with what its API offers (DLSS and FSR on
both, FSR's ray regeneration on DirectX only). This section is the first
step of the Vulkan one: the traced light inside the game, without any
vendor's upscaler or denoiser yet.

**What was found first** (prototypes outside the game, not in the
repository):

- `view.c` has all a tracer needs: every strip's vertices through the
  vertex matrix, its normals, its material, its light set, about 50,000
  triangles in a fight frame.
- The game's lights are not physical. The stage is lit by lights its own
  ceiling would block, the fighters by a set of their own, and the game
  draws its shadows apart, as volumes, with a direction that is in no
  light set. Tracing every light against everything made the picture dark.
  What kept the game's look: only the fighters cast traced shadows; the
  stage takes the game's own shadow, at the game's own darkness, by rays;
  a surface with its light painted in takes only what the fighters do to
  it.
- SDL's GPU layer has no ray tracing and does not hand its Vulkan device
  out, so ray tracing needs a device of its own.
- The GPU's ray tracing is fast enough: the tracing shader with one ray
  for each thing a pixel took 1 ms at 1280 x 960 on an RTX 5070 Ti and
  6 ms on a Radeon 8060S; the frame's acceleration structures 1.5 and
  1.1 ms. The same through DirectX 12 under Proton gave the same picture
  at about the same speed.

**What is done.** A setting, Ray tracing (settings.txt `raytracing = off |
vulkan`, the Picture tab of the game's menu and of the launcher's
Settings; off by default), for the GPU renderer. A change applies at once.

- The scene (`view.c`, `view_host_rt`). With it on, a frame handed to the
  GPU path also carries every strip's triangles as `strip_job` walks them,
  but none left out by the near plane, the culling or the screen's edges
  (`rt_export`: positions, normals, texture coordinates, colors of their
  own; a record a strip with its material, light set and state), and the
  light sets. That is about 1 ms more on the machine's thread a frame.
- Packing (`rt_scene.c`, no graphics API in it). The triangles go into
  four groups: the fighters' solid ones and by-alpha ones, the stage's
  solid ones and by-alpha ones. A fighter's triangle is one that is lit
  and not marked to take the game's shadows. The way the game's shadows
  fall is estimated from the game's shadow volumes (the fighters' outlines
  on the floor drawn out upright: the edges give the upright, the corners
  the ground plan) and the fighters' box. The game's own shadow light,
  read from its code, is to replace that estimate.
- The device (`rt_vk.c`). A Vulkan device of the ray tracing's own on the
  GPU the renderer draws with, with `VK_KHR_acceleration_structure` and
  `VK_KHR_ray_query`. Each frame, in one command buffer: the scene's
  buffers and the texels that changed are copied in, an acceleration
  structure is built for each group and one over them (the scene is in
  view space, nothing of it stands still), the tracing shader runs over
  the picture, a filter runs over its results, and they are copied where
  the processor reads them. The call waits for it. Vulkan's functions
  come through SDL's loader; its declarations are Khronos's headers in
  `runtime/vulkan` (the pinned Windows compiler has none).
- The tracing shader (`rt_light.comp`). For each pixel the eye's ray
  finds the first surface of the solid lists. For it: the game's own sum
  of its lights; and the same sum with each light's share cut by what
  rays towards it meet (fighters only, 4 rays a light, a little apart for
  a soft edge), the ambient part gathered by 4 rays over the surface (a
  free way brings the ambient color, a thing near by what it sends back,
  fading with distance over 3 of the scene's units), and for the stage
  the game's shadow light. Out goes the ratio of the two sums. A pane of
  glass before the surface (a translucent polygon of the stage with a
  highlight of its own, mostly clear, lying in the floor) mirrors the
  scene by 2 rays, weighed by its angle to the eye. The rays are the same
  every frame, so what stands still does not flicker.
- The filter (`rt_filter.comp`): each pixel's ratio is the weighted mean
  of its neighbors' within 3 pixels whose surface is about as far from
  the eye; the reflections the same within 1.
- Into the picture (`gpu_render.c`, `rt_apply.frag`). The renderer leaves
  the game's shadow volumes out, and after the solid lists the two
  results come in as textures; a pass into a second color target copies
  the picture and, where stencil bit 3 says the 3D scene is (section 73),
  writes color x ratio x (1 - the glass's share) + the mirrored color.
  The translucent list is drawn on top of that as ever, and the 2D layer
  is not touched. Ambient occlusion from the depth buffer is left out
  while ray tracing draws (the rays bring their own).
- The ray tracing's device starts with the first frame after the setting
  is turned on; it then needs the whole texel arena once, which the next
  frame brings. A GPU or driver without the two extensions: the log says
  so, and the picture is drawn as without.

Found on the way: a GPU's staging memory is of the kind that suits
writing; reading back from it (the first version did, to find the
fighters' box, and read its results from such memory too) took 125 ms a
frame. Nothing is read from it now, and the results come back through
cached memory: 10 ms.

**Checked** (the development PC; the windowed games from stage snapshots
in a headless desktop, the picture shown at a vertical blank in a fight,
internal resolution 1280 x 960 unless said, on an RTX 5070 Ti in an
external enclosure):

- Off: the picture is the same to the pixel as before the change.
- On, four stages: the fighters' shadows fall the way the game's do, with
  soft edges; the fighters shade themselves; the glass panes of one stage
  mirror the scaffold behind. A band inside a health bar: no pixel
  differs. The time to trace a frame and bring it back: 9.6 to 12.3 ms.
- A wide picture (16:9): drawn; its middle differs from the 4:3 picture
  by 0.65 steps of 255 on average (the rays' pattern falls on other
  pixels). 11.6 ms.
- Internal resolution 640 x 480: 6.5 ms. 1920 x 1440: 17.4 ms, more than
  a frame's 16.7.
- The software renderer: the same picture with the setting on as off.
- The Radeon 8060S (Mesa's RADV): the same picture as the NVIDIA card's
  but for 0.01 steps on average; 20.9 ms a frame, too slow for full speed.
- The game's menu, at the game's own pace: the row set to Vulkan during a
  fight; the file has `raytracing = vulkan`, and the picture 200 vertical
  blanks later is the same to the pixel as with the setting on from the
  start. (As fast as the machine goes, the run was over before the device
  had started: that first test showed no change, and was the test's
  fault.)
- The launcher's row, Linux and the Windows launcher under Wine: both save
  `raytracing = vulkan`.
- The Windows game under Wine: the same ray traced picture to the pixel
  as the Linux game.
- Both builds compile (Linux, Windows) with no warning.

**Known.**

- It costs about 10 ms of the window's thread a frame on a fast card, most
  of it bringing the scene to the second device and the results back
  through the processor's memory; on the Radeon it is too slow at these
  ray counts. The counts (4 ambient, 4 a light, 2 mirrored) are fixed.
- Translucent polygons (a scaffold, hair) are drawn after the traced
  light, with the game's own light and no shadow on them; they do cast
  shadows by their alpha.
- Glass is found by a rule made for one stage (and only panes lying in
  the floor of a level camera); water and metal mirror nothing yet.
- The shadows' direction is an estimate.
- The picture in motion was not looked at; the same rays every frame keep
  a still picture still, and a moving one may crawl.

**Not covered.** Windows itself, Proton (plain Wine only), a Steam Deck,
Intel graphics, a two-PC game, frame skip with it, the game at its own
pace for long. The steps to come: motion from frame to frame, the game's
own shadow light, a list of materials, the renderer owning the whole
frame so that DLSS and FSR can work, and the DirectX 12 renderer.

## 75. Ray traced light through DirectX 12 (2026-11-24)

**Why.** The maintainer decided on two ray tracing renderers, one on
Vulkan and one on DirectX 12, each with what its API offers: FSR's ray
regeneration exists for DirectX 12 only, and a player on Windows may have
a driver whose DirectX ray tracing is the better one. Section 74 put the
traced light into the game through Vulkan. This section does the same
step through DirectX 12: the same scene, the same rays, the same picture.

**What is done.** The Ray tracing setting has a third value: settings.txt
`raytracing = off | vulkan | directx`, "DirectX 12" on the row of the
game's menu and of the launcher's Settings. It is in the Windows game
only (on Windows, or under Proton, where DirectX 12 is vkd3d-proton's).

- One interface, two modules (`rt.h`, `rt_backend`: start, stop, texels,
  trace). `rt_vk.c` was written to it in section 74; `rt_dx12.c` is the
  second, compiled into the Windows game only. Everything before and
  after is shared and untouched: the scene from `view.c`, its packing
  (`rt_scene.c`), and what the renderer does with the two results
  (`gpu_render.c`, `rt_apply.frag`). The renderer itself stays SDL's GPU
  layer on Vulkan with either.
- The device (`rt_dx12.c`). A Direct3D 12 device of the ray tracing's own
  with inline ray queries (ray tracing tier 1.1). Each frame, in one
  command list, what `rt_vk.c` does: the scene's buffers and the texels
  that changed copied in, an acceleration structure built for each of the
  four groups and one over them, the tracing shader run over the picture,
  the filter over its results, and those copied where the processor reads
  them. The call waits for it. Every buffer a shader reads or writes is a
  root descriptor, so there are no descriptor heaps. Nothing is read from
  the memory the processor writes into, and the results come back through
  a readback buffer (section 74's finding).
- Buffer states. The buffers in the GPU's own memory are made in the
  common state. A frame's first use promotes one (Direct3D's rule for
  buffers), a barrier takes it on within the list, and the list's end
  puts back every buffer a barrier moved, so each list finds all of them
  in the common state without leaning on what Direct3D does to buffers
  after a list.
- Which GPU. The renderer's, by its name: DXGI's description of a GPU is
  as a rule the name Vulkan gives it. The GPUs DirectX lists are tried in
  this order: the one of that very name, those whose name holds the
  other's, then the rest; the first with ray queries is taken. When that
  is not the renderer's by name the log says which it is (the traced
  light comes back through the processor, so the picture is the same).
- The shaders (`shaders/rt_light.hlsl`, `rt_filter.hlsl`). The same two
  shaders in DirectX's language, compiled to DXIL by `tools/make_shaders.py`
  into `dx_shaders.h`, which is committed like the SPIR-V. One source for
  both was tried first: DXC can write SPIR-V from the HLSL, and the
  picture was the same, but on Mesa's RADV that shader ran 2.5 to 3 times
  slower than the GLSL one. So Vulkan keeps the GLSL, and the two must be
  changed together. The tool needs Microsoft's own build of DXC for the
  DXIL (the README says where to get it): the one CachyOS packages
  (1.10.2605) failed inside the compiler on every shader, even a one-line
  one, and a DXIL that is not signed is refused by Windows, so the tool
  refuses to write one.
- Never in the native Linux game (the maintainer's decision: the Linux
  version does not use DirectX). DirectX 12 goes with the game, not with
  the launcher's system: a Linux launcher builds either the Windows game
  (for Steam, where it runs under Proton and DirectX 12 works) or a native
  Linux program. So the word `directx` is read by every build, because a
  Linux launcher keeps the settings of the Windows game too, and
  `options_directx` says whether the row offers it: always in the Windows
  game and the Windows launcher; in a Linux launcher when its game is the
  Windows one (`build.json`; before a build, the kind the next build will
  be); never for the native Linux game. That game reads the word as off
  and says so in the log; its menu and its launcher show Off for it, pass
  DirectX 12 by, and write `off` or `vulkan` at the next save. The help
  text has its sentence about DirectX 12 only where the value is offered.
- Builds: `rt_dx12.c` and Windows' own `d3d12`, `dxgi` (and the
  compiler's `dxguid`) in the Windows build only, in `CMakeLists.txt` and
  in the builder (`tools/vf4build`). The Direct3D declarations are the
  pinned compiler's own (llvm-mingw's headers); nothing was added to the
  repository for them.

Found on the way:

- The Ray tracing row's help text had been two lines too long for the
  launcher's help box since section 74 (the launcher's window is smaller
  than the game's menu, and section 74 looked at the saved file, not at
  the box). It is shorter now and fits in both launchers, with the
  Windows one's sentence more.
- The renderer's log line said "ray tracing (...) on" and the renderer's
  GPU, which is wrong when the ray tracing's device is another: it now
  says that the ray tracing started and where the picture is drawn.

**Checked** (the development PC; the Windows dev game under Proton
Experimental, started with umu-run in a headless desktop from the stage
snapshots; the picture shown at a vertical blank in a fight, internal
resolution 1280 x 960 unless said, on an RTX 5070 Ti in an external
enclosure; "against Vulkan" is the same Windows game with
`raytracing = vulkan`):

- Four stages: the DirectX picture against the Vulkan one differs in 487,
  31, 467 and 153 of 1,228,800 pixels, by 2 steps of 255 at most. To
  trace a frame and bring it back: 10.0 to 14.6 ms, against 9.5 to 13.8
  for Vulkan in the same runs (DirectX is 0.5 to 1.1 ms slower here).
- The Windows game's Vulkan ray traced picture under Proton: the same to
  the pixel as the Linux game's.
- Internal resolution 640 x 480: 142 pixels differ, 2 steps at most;
  6.8 ms (Vulkan 6.0). 1920 x 1440: 2,709 pixels, 4 steps at most;
  25.4 ms (Vulkan 24.6). A wide picture (16:9, 1708 x 960): 60 pixels,
  1 step at most; 16.5 ms (Vulkan 14.5).
- The Radeon 8060S (Mesa's RADV under vkd3d-proton). In the headless
  desktop the Radeon could not show the Windows game's window under
  Proton (the renderer fell back to the NVIDIA card, by its own rule), so
  the renderer stayed on the NVIDIA card and DirectX was shown the Radeon
  alone (DXVK's filter): the ray tracing took it as "another GPU", said
  so in the log, and traced a frame in 20.2 ms (Vulkan on that Radeon:
  20.9 ms, section 74). Against the NVIDIA picture 3,111 pixels differ,
  27 of them by more than 5 steps, 8 steps at most.
- The game's menu, at the game's own pace, in a fight: Off to DirectX 12;
  DirectX 12 to Vulkan; DirectX 12 to Off. Each time the file has the
  value, and the picture 200 vertical blanks later is the same to the
  pixel as with that value from the start.
- No DirectX ray tracing (the Windows game under plain Wine, whose
  Direct3D 12 has none): the log says that it could not start and why,
  and the picture is the same to the pixel as with the setting off.
- The native Linux game with `raytracing = directx` in the file: the log
  says it takes it as off, and the picture is the same to the pixel as
  off. Its menu's row shows Off; Right once makes it Vulkan, the file then
  says vulkan, and the picture 200 vertical blanks later is the same to
  the pixel as Vulkan from the start. Off and Vulkan: the same to the
  pixel as before this change.
- The launchers, by the file each saved after keys on the row, and by
  pictures. The Windows one (under Wine): Off, Vulkan, DirectX 12, Off;
  it saves `directx` and shows "DirectX 12" for a file that has it. The
  Linux one with the Windows game (one built, and none built yet): the
  same, and a file that says directx is kept. The Linux one with a native
  Linux game: Right twice from Off is Off again, a file that says directx
  shows as Off, and Right from it saves vulkan. The help text fits its
  box in all of them, with and without its sentence about DirectX 12.
- After that change the Windows game under Proton again: the DirectX
  picture, and the menu's switches to and from DirectX 12, the same to
  the pixel as before it.
- Both development builds compile with no warning. The builder's own
  Windows build (`tools/make_game.py`, into a folder of its own) compiles
  and links with the new file and libraries, and its game's DirectX
  picture is the same to the pixel as the development build's.

**Known.**

- Windows itself has not run it. Under Proton, Direct3D 12 is
  vkd3d-proton, which does not check a program's use of the API as
  Windows' own debug layer does; the buffer states above were written to
  Direct3D's documented rules, and that is all that stands behind them
  until someone runs it on Windows.
- The cost is section 74's: most of the time goes into bringing the scene
  to a second device and the results back through the processor's memory,
  and the ray counts are fixed (too slow on the Radeon).
- Two shader sources to keep the same (GLSL and HLSL); the check is the
  two pictures against each other, as above.
- The GPU is matched by name. A GPU whose DXGI description differs from
  its Vulkan name is still found when one name holds the other, or as the
  first with ray queries; with two such GPUs it may be the other one (the
  picture is the same, the speed may not be).
- Everything section 74 lists as known holds for this one too.

**Not covered.** Windows itself; AMD's and Intel's Windows drivers; a
Steam Deck; a GPU with DirectX 12 but ray tracing tier 1.0 only (the
message for it was read, not seen); a two-PC game; frame skip with it; a
long run at the game's own pace. Still to come, for both: a quality
setting (section 76), motion from frame to frame, the
game's own shadow light, a list of materials, the renderer owning the
whole frame so that DLSS and FSR can work, FSR's ray regeneration on the
DirectX side.

**Decided for those steps** (the maintainer, 2026-11-24). The launcher may
download what a feature needs and the repository cannot hold (a vendor's
DLL such as DLSS's or FSR's, a compiler), as long as the player is told
before anything is downloaded: what it is, from whom, and how large, as
the launcher already does for the compiler of the first build and for an
update. Nothing of this section needs a download: the DirectX shaders are
in the repository ready made, and Direct3D 12 is Windows' own (under
Proton, vkd3d-proton's).

## 76. Ray tracing quality (2026-11-25)

**Why.** Sections 74 and 75 trace every pixel of the picture with fixed ray
counts, which a Radeon 8060S cannot do in a frame's time. The maintainer
asked for a quality setting next; the plan had been "fewer rays for a
slower card".

**Measured first** (the Linux game, Vulkan, one stage at 1280 x 960, as
fast as the machine goes; the ray counts set through a test switch that is
gone again; the time is from handing the frame to the ray tracing's device
until it is done):

| rays a pixel: around, a light, a mirror | Radeon 8060S | RTX 5070 Ti (external enclosure) |
|---|---|---|
| 4, 4, 2 (sections 74 and 75) | 12.0 ms | 7.7 ms |
| 2, 2, 1 | 9.2 | 7.2 |
| 1, 1, 1 | 7.8 | 7.0 |
| none: the eye's ray and the filter alone | 5.5 | 6.9 |
| none, at 640 x 480 | 2.4 | 4.3 |
| none, at 1920 x 1440 | 8.0 | 11.4 |

- On the Radeon the rays are about half of the time, and the rest goes
  with the number of pixels. On the NVIDIA card the rays are a tenth of
  it. What costs there is carrying data over the enclosure's cable: the
  scene in (180 bytes a triangle: 9 MB a frame with section 74's 50,000
  triangles) and the two results back (8 bytes a pixel, 10 MB at
  1280 x 960). By the three sizes above that is about 2.7 GB a second,
  an estimate: the eye rays and the filter are in those times too. A card
  inside the PC has several times that, so this share should be smaller
  there; none was here to measure.
- So fewer rays alone would have taken a third off the Radeon's time and
  next to nothing off the other card's. Fewer traced pixels take it off
  both. The setting does both.
- Bounced light (an ambient ray brings the light of what it meets) costs
  0.2 ms of the Radeon's 7.8: it stays on at every level.
- The first traced frame of a run took 0.4 to 0.5 s on the Radeon when
  Mesa's shader cache was empty (each test batch has a cache of its own):
  RADV compiles the shaders it builds acceleration structures with at
  their first use. It is once, not a frame's cost, but it was inside the
  mean the log gives, and the game ran flat out, where the renderer's own
  frames queue up on a slow GPU and the tracing waits behind them. Section
  74's 20.9 ms for the Radeon was both: at the game's own pace, without
  the first frame, the same picture takes 17.5 ms.

**What is done.** A setting, Ray tracing quality (settings.txt
`rayquality = low | medium | high`; a row under Ray tracing on the Picture
tab of the game's menu and of the launcher's Settings), for both ray
tracers. A change applies at once. Medium is the default.

| | traced pixels | rays: around, a light, a mirror | the filter reaches |
|---|---|---|---|
| High | every pixel of the picture | 4, 4, 2 | 3 pixels |
| Medium | a quarter (half the internal resolution each way) | 4, 4, 2 | 2 traced pixels (4 of the picture's) |
| Low | a quarter | 1, 1, 1 | 3 traced pixels (6 of the picture's) |

- The levels are a table in `gpu_render.c` (`rt_levels`, an `rt_quality`
  each: the rays, the bounce, the divisor, the filter's reach).
  `gpu_render_raytracing` takes the level with the backend; the next
  frame uses it.
- Fewer traced pixels. The tracer is not changed: it is asked for a
  picture half the size each way, so its eye rays go through the middle
  of each 2 x 2 block of the picture's pixels, and the filter works on
  that smaller picture. Besides the two results a backend now hands back,
  when asked, how far each eye ray went (`reach`; the filter had it
  already). Three small buffers come back instead of two large ones:
  3 bytes a picture pixel instead of 8.
- The fill (`shaders/rt_fill.frag`, the renderer's own GPU). Each pixel of
  the picture takes the four traced pixels around it by how near they lie
  (as any enlarging does), but only those whose eye ray went about as far
  as this pixel's own surface is (the filter's measure: within 4 % and a
  little). So a fighter's light does not run over the outline onto the
  floor behind, and the floor's shadow not onto the fighter. How far the
  pixel's surface is comes from the picture's own depth buffer, as the
  ambient occlusion of section 73 reads it. If none of the four fits
  (something thin, or an outline the tracer drew a pixel aside), the one
  of the sixteen around that comes nearest is taken alone; if nothing was
  traced near, the pixel keeps the game's own light.
- Two passes for it, because a pass cannot read the depth buffer and use
  its stencil at once: the fill writes the lit picture into the second
  color target for every pixel with something drawn; the pass that goes
  on then first copies the picture as it was back where stencil bit 3 is
  not set (the 2D layer over the scene), and the translucent list follows
  as ever.
- High goes the way of sections 74 and 75, through the same pass as
  before: its picture is theirs to the pixel.
- The filter's reach is a constant of the shaders now
  (`rt_consts.counts[1]`; the GLSL and the HLSL filter both), 1 to 3
  traced pixels each way. Medium's is 2: with 3, as first tried, a
  shadow's edge under a foot was visibly softer than High's.
- A device whose depth buffer no shader can read (none here; the ambient
  occlusion's own condition) traces every pixel with the level's rays,
  and the log says so.
- The log says what is traced, at the start and at each change
  ("ray tracing quality medium: 640 x 480 traced for a picture
  1280 x 960"), and the settings line has the level. The mean time it
  gives at the end leaves the first frame after each start out and names
  it apart.

Why Medium is the default: its picture is hard to tell from High's (below),
and it is what a mid-range chip can do in a frame's time. Ray tracing
itself stays off by default.

Found on the way: the launcher's `--shot` read the window back after
presenting it, and what comes back then is an older frame's picture (one
frame back on Linux here, three under Wine). So a `--shot-keys` picture
lagged that many keys behind what the keys had done (seen in section 75's
tests and not explained then; the saved files were right, and those were
what was checked). The last frame is now drawn and read back
without being presented: ten times Down shows the tenth row in both
launchers, and their two pictures are the same to the pixel.

**Checked** (the development PC; the windowed games from stage snapshots
in a headless desktop; the picture shown at a vertical blank in a fight;
1280 x 960 unless said; NVIDIA is an RTX 5070 Ti in an external enclosure,
the Radeon an 8060S with Mesa's RADV):

- High against the build before this change: no pixel differs, four
  stages; Off the same.
- The cost, at the game's own pace (60 a second), to trace a frame and
  bring it back, in ms:

  | picture, stage | NVIDIA: High | Medium | Low | Radeon: High | Medium | Low |
  |---|---|---|---|---|---|---|
  | 640 x 480, stage 3 | 5.8 | 5.3 | 4.9 | 6.6 | 4.4 | 3.8 |
  | 1280 x 960, stage 3 | 10.0 | 6.3 | 6.1 | 17.5 | 6.7 | 4.6 |
  | 1280 x 960, stage 21 | 9.0 | 5.8 | 5.6 | 13.0 | 5.2 | 4.0 |
  | 1280 x 960, stage 22 | 11.7 | 8.1 | 7.7 | 18.3 | 6.7 | 4.9 |
  | 1920 x 1440, stage 3 | 16.7 | 8.4 | 7.9 | 34.0 | 11.2 | 6.4 |

  Where a level took longer than a frame the game drew fewer pictures in
  the same time: the Radeon at High drew 828 of 967 at 1280 x 960 and 464
  of 957 at 1920 x 1440, the NVIDIA card at High 614 of 958 at
  1920 x 1440 and 868 of 936 on stage 22. Medium and Low drew all of them
  in every row.
- The pictures, four stages. Medium against High differs by 0.45 to 0.85
  steps of 255 on average, Low by 0.62 to 1.2. By more than 26 steps
  differ 110 to 699 of 1,228,800 pixels at Medium and 277 to 2,870 at
  Low; on the stage looked at for it they lie on the edges of the
  fighters' shadows, which are a little softer. Looked at
  enlarged three times on three stages: a foot on the floor, a fist
  before the background, a shadow's edge, the glass panes. No light runs
  over an outline at any level. A thin beam mirrored in the glass is a
  row of dots at High, a soft streak at Medium and a clean line at Low
  (one ray, no roughness).
- Motion, by numbers only: of eight pictures in a row, how much the
  traced light's share changes from one to the next: 0.21, 0.21 and 0.20
  steps for High, Medium and Low on one stage, 0.14, 0.15 and 0.15 on
  another. Fewer traced pixels add no change from frame to frame by this
  measure. Nobody has watched it move.
- 640 x 480 (320 x 240 traced), 1920 x 1440 (960 x 720) and a wide
  picture (16:9, 1708 x 960; 854 x 480 traced): drawn, Medium against
  High 0.62, 0.40 and 0.76 steps on average.
- The Radeon's pictures against the NVIDIA card's, each level: 0.01 steps
  on average, as section 74 found for High.
- The game's menu, at the game's own pace, in a fight: Right on the row
  from Medium, Left from Medium, Left from High. Each time the file has
  the level and the picture 200 vertical blanks later is the same to the
  pixel as with that level from the start. With the Software renderer the
  row does nothing, as Ray tracing's.
- The launchers (Linux, and the Windows one under Wine), by the file each
  saved: Right from nothing saved gives high, Left low, Right three times
  medium again; a file that says high, then Left, medium. The help text
  fits its box in both, with a line to spare in the launcher's.
- DirectX 12, the Windows game under Proton, two stages: at each level
  the DirectX picture differs from the Vulkan one in 173 to 579 of
  1,228,800 pixels, by 3 steps of 255 at most (High: 487 and 467 pixels,
  as section 75 had). The Windows game's Vulkan picture at each level:
  the same to the pixel as the Linux game's. At the game's pace: Vulkan
  9.8, 6.3 and 6.0 ms; DirectX 10.6, 7.0 and 6.6. The row in the Windows
  game's menu with DirectX 12 on: Medium to High, Medium to Low, High to
  Medium, each the same to the pixel as that level from the start.
- Both development builds compile with no warning.

**Known.**

- Medium and Low are softer where the traced light changes within a few
  pixels: a shadow's edge near the foot that casts it, a thin thing's
  shadow. Something thinner than two picture pixels may be missed by
  every eye ray and then takes the light of what lies nearest in depth.
- Low is hardly cheaper than Medium on the NVIDIA card in its enclosure
  (0.2 to 0.6 ms): what is left there is carrying the scene over, which
  no level changes. A triangle is sent twice over (its corners for the
  acceleration structure, and again with its normals and colors for the
  shader); sending less is its own step.
- The levels' numbers were chosen on two GPUs. A Steam Deck or an older
  card may want a level below Low (a third of the resolution).
- The first traced frame after Mesa's shader cache was emptied (a new
  driver, a first run) takes about half a second on the Radeon. Not
  looked into further: it is RADV's own work, once.
- Everything sections 74 and 75 list as known holds here too, but for the
  fixed ray counts.

**Not covered.** Windows itself; a GPU inside the PC; a Steam Deck; Intel
graphics; a device whose depth buffer cannot be read (the message for it
was read, not seen); a two-PC game; frame skip with it; the builder's own
build (no new source file: the development builds have the same ones); the
picture in motion, watched. Still to come: motion from frame to frame, the
game's own shadow light, a list of materials, the renderer owning the
whole frame so that DLSS and FSR can work, FSR's ray regeneration on the
DirectX side.

## 77. The board's clock is the PC's, and the log names each match's stage (2026-11-25)

**Why** (issue 3 again). The laptop's owner tried Frame skip (section 71):
"works for a match and then everything goes back to slow motion", with
pops in the sound. His log: Frame skip was on throughout (half of the
game's frames drawn in both matches); in the first match the machine's
thread took 15.3 to 15.6 ms a frame and was never behind, in the second
about 21.8 ms; the sound's queue was filled in the first and empty in the
second, so the pops go with the slow motion. (The count of "times behind"
in the log's machine line runs on from the start; it is not a window's.)

The log did not say which stages those were. Worked out: a one-player
game takes its opponents, each with a stage, from a table of fifteen
pairs, starting at the day of the month (`cpu_opponent_pick` in the game's
code: the pair at (round + day) modulo 15). The machine's clock was a
fixed date, 2004-11-10 (section 16: so that runs repeat). So on every PC
and every day a one-player game began on stage 17 and went on to stages 21
and 20, two of the three stages that cost most (section 78), and the
log's "at most 41,782 triangles" fits stage 17. His log of the day before
has the same step at the second match (about 20 ms, then 25 and more),
which was not followed up then.

A cabinet has a clock with a battery, set by its operator. A PC has one
too. The maintainer asked for it to be used, and for the log to say which
stage a match is on.

**What is done.**

- *The clock.* `game_machine_clock` (before `game_machine_init`) gives
  what the board's clock shows as the machine starts, in its own count,
  seconds from 1950-01-01 00:00:00; it goes into `hw.rtc_base` after
  `hw_init`, from where the boot and the clock chip's registers have it,
  and it runs on with the machine as it always did. The game window gives
  it this PC's local date and time (SDL's `SDL_GetCurrentTime` and
  `SDL_TimeToDateTime`). Nothing in `runtime/machine` changed: the fixed
  date is still what the machine has by itself, and what the headless
  program runs with.
- *A two-PC game* has the host's on both PCs: both machines must be the
  same from the first instruction. It is one more field of the session's
  start (`session_start.clock`), four bytes in WELCOME and in LATE (both
  PCs run the same build, section 50, so the packets could grow).
- *A snapshot* (`--resume`) has its own: the clock is part of the machine.
- *For tests:* `--clock fixed` gives the fixed date, with which every run
  from boot is the same run, and `--clock YYYY-MM-DDThh:mm:ss` any date
  from 1950 to 2085. The artwork's run (`--art`) keeps the fixed date: its
  scenes are found by their vertical blanks.
- *The log* says it: "clock: the board's clock started at 2026-11-25
  09:23:57 (this PC's date and time)", or the host's, `--clock`, the fixed
  date, the snapshot's.
- *Each match.* When the game's mode becomes GAME MODE.GAME_LOAD the log
  has "match: stage 21; player 1's character 14, player 2's 10 (the
  game's own numbers), loading at vertical blank 4148"
  (`game_machine_match` reads the game's `match_stage` and
  `player_chara`). Numbers, not names: the game's tables of names are not
  in this repository.

The game's own date code (`clock_to_calendar`, its library's) takes the
count as unsigned, so a date after January 2038 is no trouble to it; every
fourth year is a leap year to it, which holds to 2099.

**Checked** (the development PC; the windowed games in a headless
desktop, from boot, as fast as they go, player 1 alone starting a game and
taking the first character):

| the clock | the log's first match |
|---|---|
| this PC's, 2026-11-25 | stage 17, against character 14 (25 and 10 are the same place among fifteen: today gives the old order) |
| `--clock fixed` | stage 17, against character 14 |
| `--clock 2026-11-26T12:00:00` | stage 21, against character 10 |
| `--clock 2026-12-03T08:00:00` | stage 5, against character 4 |
| `--clock 2026-11-19T23:59:50` | stage 4, against character 11: the 20th's, the day having turned during the boot |

Each is the pair the game's table has at that day.

- `--clock fixed`: the machine's fingerprints every 500 vertical blanks to
  6,000, twelve of them, are those of release 0.5.22's game; two runs gave
  the same.
- The Windows game under Proton: the clock line had the PC's local time
  (09:23:57, between two readings of `date` at 09:23:40 and 09:24:01), and
  the fixed date with `--clock fixed`.
- A two-PC game, the Linux game hosting and the Windows game joining under
  Wine, twice: both logs have the same clock (the host's log "this PC's
  date and time", the other's "the host's"), 154 fingerprints agreed, and
  both name the same stages for the two matches: 21 and then 20 in the
  first game, 21 and then 17 in the second, fourteen minutes later by the
  clock (a run from boot is not the same run every time any more).

**Known.**

- A run from boot is no longer the same run every time. A test that needs
  that passes `--clock fixed`; a run from a snapshot is as it was. The
  checks from boot of earlier sections had the fixed date without asking.
- As fast as it goes, the game's clock runs ahead of the PC's (it counts
  the machine's cycles); at the game's own pace the two keep step.
- When the game starts again within a run (test mode's EXIT) the BIOS's
  word of the clock is the machine's start's, not the restart's, as before.
- What else the game does with the date was read in its code, not looked
  at: a stage has a clock whose hands it sets by the time
  (`hbr_clock_update`), and its network code compares times of day.

**Not covered.** Windows itself; a PC joining a game going on (LATE's
field is written and read, not run); a PC whose date SDL cannot read (the
fixed date then, and the log says so).

## 78. A stage's reflection drawn by the graphics chip (2026-11-25)

**Why.** With section 77's finding the question was what stages 21 and 20
cost. Measured first, on release 0.5.22's way of drawing (the development
build of that morning): every one of the 24 stages, the windowed game from
a snapshot as the stage's intro begins (the fighters stand), held to two
cores and their second threads, the laptop's settings (GPU renderer at 1 x,
16:9, Lighting GPU), as fast as it goes; the machine's thread, ms a frame,
Frame skip off and on:

| stage | off | half | | stage | off | half |
|---|---|---|---|---|---|---|
| 17 | 3.79 | 2.38 | | 2 | 3.14 | 2.30 |
| **21** | **5.29** | **4.46** | | 3 | 2.89 | 2.23 |
| **20** | **5.34** | **4.10** | | 23 | 2.89 | 1.88 |
| **19** | **6.09** | **4.88** | | 0 | 2.79 | 2.00 |
| 22 | 4.24 | 2.72 | | 1 | 2.66 | 1.88 |
| 4 | 3.68 | 2.40 | | 11 | 2.42 | 1.65 |
| 16 | 3.50 | 2.17 | | 8 | 2.16 | 1.57 |
| 5 | 3.45 | 2.28 | | 6, 7, 9, 10, 12 to 14 | 1.7 to 1.9 | 1.4 |

(Stages 15 and 18, which the one-player game never visits, took 2.96 and
3.23 ms, and Frame skip took nothing off 15.) Stages 19, 20 and 21 cost
about twice what a stage of their triangle count does, and Frame skip took
16 to 23 % off them against 28 to 38 % off the others.

Those three have water. The game draws the scene a second time each frame,
mirrored, into a texture (512 x 256, sixteen bits a pixel), and puts that
texture on the water: 30,000 triangles on stages 19 and 21, as many as the
screen's own. A pass into a texture was drawn by `view.c`'s rasterizer on
the processor whatever drew the screen (section 20's GPU renderer left it
there), with the processor's lights whatever Lighting said (section 67),
and Frame skip did not skip it (section 71). On a two-core laptop that is
most of the stage's cost; the graphics chip, by the laptop's logs, has
time to spare.

**What is done.** With the GPU renderer, the graphics chip draws such a
pass, and nothing of it goes through the processor's rasterizer or into
texture memory.

- *The view* (`view.c`). `view_host_gpu_passes` (on: the renderer's wish).
  A pass into a texture of the one kind the game makes (5, 6 and 5 bits a
  pixel, its rows as long as it is wide) leaves `scene_draw` as a pass to
  the screen does (`gpu_export`), with `view_gpu_frame.target` saying which
  of the host's four target pictures it is for (`gpu_tg`: the place in
  texture memory each stands for, and its size). Lighting GPU lights it
  like the screen. `target_store` is not called: texture memory keeps what
  it had.
- *Who uses it.* A state of a later pass whose texture is exactly that
  place (the same address and size, 5-6-5, not twiddled, one level, no
  codebook) is marked (`view_gpu_state.target`), and its texture does not
  go to the texel arena. A texture that reads the place any other way
  sends the place back to the processor's rasterizer for the rest of the
  run, with a line in the log (none was seen). Once the game writes a
  texture of its own over the place (texture memory's checksum there is no
  longer what it was as the pass was drawn), the picture is not what that
  memory holds, and the state takes texture memory's texels again.
- *The renderer* (`gpu_render.c`). The pass comes from the machine's
  thread just before the frame that uses it and is kept for it (`pend`,
  then the frame's `packet`): the two are drawn together or not at all. It
  is drawn first, by the same steps as a frame (background, the opaque and
  punch-through lists, the volumes' shadows, the translucent list: the
  frame's code for them is now three functions both use), at one pixel a
  pixel, into a target picture of its own with its own depth buffer and
  its own vertex and state buffers. The scene's fragment shader has four
  more inputs, the pictures; where a state names one, a texel is that
  picture's pixel, cut to 5, 6 and 5 bits and widened again as texture
  memory and the texture's decoder would have done (`target_store`,
  `texel16`), so the water takes the same colors as before.
- *The wide screen.* Section 65 draws such a pass wider and kept a second
  texture twice as wide for the screen. Here the target picture is simply
  the wider one, the state says how many columns lie beyond each side
  (`target_ox`), and a clamped texture coordinate may run that far past
  the texture's edge.
- *Frame skip and frames run again.* The game sends a screen's lists, then
  those of the pass that screen uses, renders the pass and then the
  screen. So in the game window a pass's lists are now taken when its
  screen's are (`lists_begin`): not for a frame Frame skip leaves out, nor
  for a frame run again. That holds for the processor's passes too (the
  software renderer, and ray tracing below), which sections 56 and 71 drew
  for every frame.
- *Texture memory.* The game never reads it (section 55); the machine's
  fingerprint is main RAM and the registers. With the GPU renderer the
  reflection is no longer in it, so texture memory is not the same on
  every PC of a two-PC game any more, and sections 56 and 71's "the
  machine is the same" now means what the game can see of it.
- *Ray tracing wanted:* the machine's thread draws the passes as before.
  The ray tracing's device has the texel arena, not these pictures.
- *For tests:* `--cpu-passes` draws them the old way with the GPU renderer.
- *The log* at the end: "GPU renderer: 3068 passes into textures drawn (a
  stage's reflection); 0 frames used one not drawn for them".

**Checked** (the development PC; the windowed games from stage snapshots
in a headless desktop; internal resolution 1 x and 4:3 unless said; the
picture shown at a vertical blank):

- The cost, the machine's thread in ms a frame, as in the table above
  (another batch: such numbers move by a tenth between batches):

  | stage | the processor's pass (`--cpu-passes`): off, Frame skip | the graphics chip's: off, Frame skip |
  |---|---|---|
  | 21 | 6.04, 3.25 | 2.98, 1.94 |
  | 20 | 5.10, 3.38 | 3.14, 2.23 |
  | 19 | 6.20, 3.88 | 3.57, 2.56 |
  | 17 | 3.92, 2.79 | 3.84, 2.80 |
  | 3 | 3.19, 2.24 | 3.12, 2.17 |

  With Frame skip a water stage now costs less than stage 17, on which the
  laptop held full speed. (The processor's pass with Frame skip is cheaper
  than the first table's too: half of the passes are no longer drawn.)
- At the game's own pace on the Radeon 8060S, 16:9 at 2 x, stage 19: 5.78
  ms a frame with the processor's pass, 4.40 and 4.51 with the graphics
  chip's; after the first ten seconds every frame drawn was shown, either
  way.
- The pictures, the graphics chip's pass against the processor's. Where
  the water is in view (stages 19 and 20 in the fight, 18, and the
  Shrine's intro, whose camera goes over the pond): 4,000 to 34,000 of
  307,200 pixels differ, by 0.01 to 0.2 steps of 255 on average; they lie
  in the water. Looked at enlarged three times on stage 19 (the water under
  the floor's grid) and on the Shrine at 21:9 (the bridge's reflection; the
  far right, which only the wide pass holds): the same things in the same
  places. In the Shrine's fight, where the pond is out of view, no pixel
  differs.
- 16:9 and 21:9, stage 19: 33,000 and 60,000 pixels differ, 0.14 to 0.22
  steps on average.
- Nothing else changed: three stages without a pass at 2 x, two pictures
  each, are release 0.5.22's to the pixel, and stage 19 with
  `--cpu-passes` is too.
- Frame skip, stage 19: the pictures of the frames that are drawn are the
  same to the pixel as without Frame skip, with either pass; the log's
  "used one not drawn for them" stayed 0 in every run. The software
  renderer with Frame skip, stage 19, four vertical blanks: release
  0.5.22's pictures to the pixel (the pass goes with its screen).
- The machine is the same: its fingerprints every 250 vertical blanks to
  9,000 on stages 19 and 21 are the same ten with the graphics chip's
  pass, the processor's, Frame skip, and the software renderer. The two
  two-PC games of section 77 were on stages 21 and 20 and on 21 and 17,
  the host with the graphics chip's pass and the other PC with Frame skip
  as well: 154 fingerprints agreed in each, and neither log had a frame
  that used a pass not drawn for it.
- Frames run again (`--synctest 8`, stage 19): every replay the same, and
  the pictures the same to the pixel as without.
- Lighting GPU with the graphics chip's pass, stage 19: the same to the
  pixel as Lighting CPU.
- Ray tracing on, stage 19: no pass drawn by the renderer, and the picture
  the same to the pixel as with `--cpu-passes`.
- The Windows game under Proton, stage 19: its picture with the graphics
  chip's pass is the Linux game's to the pixel, and with the processor's
  too.
- Both development builds compile with no warning.

**Known.**

- The reflection is drawn at the size the game asks for, 512 x 256,
  whatever the internal resolution: as before.
- Its pixels differ a little from the processor's rasterizer's where two
  triangles of the mirrored scene meet, as the numbers above say.
- The scene's fragment shader grew (four inputs, a branch a texel). On the
  two GPUs here it did not show; an Intel chip was not here to try.

**Not covered.** The laptop itself and Intel graphics; Windows itself; a
Steam Deck; the picture in motion, watched; ray tracing switched on or off
in the menu on a water stage (the passes change hands then: reasoned,
not run); a renderer started again on another GPU on a water stage; stage
15's two passes (no state used them in the frames looked at); a PC joining
a game going on; the builder's own build (no new source file).

## 79. Lighting on the GPU by default (2026-11-25)

**Why.** Section 67 made the GPU renderer's lights an option, the
processor's by default, until it had been seen on more graphics chips.
Since then it has run on a GeForce RTX 5070 Ti and a Radeon 8060S on
Linux, the same two with the Windows game under Proton and Wine, and on a
player's Intel HD Graphics 620 on Windows (issue 3), with no fault in the
picture. It takes about a tenth off the game's thread on that laptop, and
costs a fast PC nothing. The maintainer asked for it to be the default.

**What is done.**

- `settings_defaults` gives `lighting = gpu`.
- A settings file says `lighting = cpu` on every PC that ran 0.5.19 to
  0.5.23, because the game writes every setting out, not because the
  player chose it. So the file's layout number goes to 3 (section 70 made
  it 2 for Window size's default): a file of an earlier layout that says
  `cpu` is read as `gpu`, once. The next time the file is written it has
  layout 3, and from then on `cpu` means the player's choice and stays.
  A player who had chosen GPU and gone back to CPU on purpose gets GPU
  once more and has to set CPU again; the two cannot be told apart in an
  old file.
- The row's help text and the file's comment name GPU first, and say what
  CPU is for: a picture that looks wrong with GPU.
- The software renderer's lights are the processor's, as ever.

**Checked.** A game with no settings file, with a file of layout 2 that
says `cpu`, and with one of layout 3 that says `cpu`: the log's settings
line has lighting gpu, gpu and cpu. The launcher, given the layout 2
file: its page shows GPU, and the file it saves has layout 3 and `gpu`.
The picture with the GPU's lights against the processor's on a stage where
they differ at all (stage 22 at 1280 x 960): 96,271 of 1,228,800 pixels,
by one step of 255, none by more.

**Known.** Not run on a Steam Deck, on AMD graphics under Windows itself,
or on an older graphics chip than these. A test that writes its own
settings file with `lighting = cpu` must give it `layout = 3` too, or the
game reads it as gpu (the test kits were changed).

## 80. The traced light's rays, spread evenly (2026-11-25)

**Why.** The maintainer watched the ray traced picture of sections 74 to 76
move, which nobody had: "The shadows are very noisy either way whether it
is medium or high." Section 76's numbers for motion had not shown it. What
the tracer did: four rays for each use (the light around, the game's
shadow, each light on a fighter), their directions drawn by a hash of the
pixel, the same every frame so that a still picture does not flicker; one
smoothing pass over two or three pixels each way; nothing kept from the
frames before. Rays drawn by chance clump and leave gaps, the pass
averages a few clumps, and what is left stays where it is on the screen
while the scene moves under it.

**How it was measured.** A reference of the same frame with 48 rays for
each use (a measuring switch, gone again), through the same smoothing
pass, traced at each level's own size. A level's error is the root mean
square of its picture's difference from that reference, in steps of 255,
over the whole 1280 x 960 picture at one vertical blank of a fight. Three
stages.

**What is done** (`rt_light.comp`, and `rt_light.hlsl` the same):

- *Rays that are placed, not drawn.* The n rays of a use lie on a spiral
  over the light's disk (or the hemisphere), one in each ring of equal
  area (`ray_at`). Each pixel turns its spiral by an angle of its own and
  shifts its rings by an offset of its own, and these two come from
  interleaved gradient noise: a pattern whose values over any small
  neighborhood of pixels are spread evenly. So the pixels the smoothing
  pass averages hold rays that cover the disk evenly between them. Each
  use adds its own constant to both, so that a pixel's uses do not share
  directions. The pattern is the same every frame, as before.
- *More rays in a soft edge.* Where some of the game's shadow rays pass and
  some do not, the pixel is in a shadow's soft edge: it takes twice as
  many again (12 in all at Medium and High). The same for each light on a
  fighter. Elsewhere all rays agree, and more would tell nothing.
- *No rays for nothing.* A surface with painted light (most of a stage)
  took four shaded rays for the light around it and then used nothing of
  them but how open it is to the fighters, and that only where the game's
  shadow falls on it. Now it takes three times the rays there, none of
  them shaded (what they meet is not asked for its color), and none at all
  elsewhere.

**Tried and not kept**, each measured:

- *A wider or flatter smoothing pass.* Against a reference with the pass
  at its smallest, every wider or flatter setting had more error: it blurs
  more than it cleans. A narrower one at High let the rays' pattern show
  as a faint regular weave. The pass is as it was.
- *More rays for the light around* where something is near (three times
  as many, shaded): a quarter less error again, for 22 % more time at High
  on the NVIDIA card and about 40 % more at Medium on the Radeon (one run
  of each, and the Radeon's runs swing). The same rays unshaded: no less
  error at all, and nearly the same time on the Radeon.
  The same kept to the fighters and their floor: no less error.
- *A bounce reading a smaller level of the texture it meets:* no less
  error.

So the grain that is left is in the color the rays for the light around
bring back from the lit scenery, and neither a cheaper ray nor a blurrier
texture touches it. Its remedy is to average over frames, or in the scene
itself, which needs to know how every surface moved since the last frame.
That is the next step, not this one.

**Checked** (the development PC; the windowed game from stage snapshots in
a headless desktop; 1280 x 960; NVIDIA is an RTX 5070 Ti in an external
enclosure, the Radeon an 8060S):

- The error against the reference, release 0.5.23 and now:

  | stage | High | Medium | Low |
  |---|---|---|---|
  | 3 | 0.626, 0.338 | 0.674, 0.403 | 1.197, 1.131 |
  | 22 | 0.937, 0.707 | 1.094, 0.823 | 2.159, 1.992 |
  | 21 | 0.675, 0.501 | 0.792, 0.568 | 1.545, 1.419 |

  A quarter to nearly half less at Medium and High, 6 to 8 % less at Low
  (one ray a use: no spiral to place, and no telling a soft edge).
- Looked at enlarged, the traced light alone and the picture: a fighter's
  leg and the floor under it at Medium had blotches that are now faint,
  and a shadow's edge on the floor is smooth where it was lumpy.
- Ray tracing off: the same to the pixel as 0.5.23 on the three stages.
- The mean brightness of the picture against the reference moved by less
  than 0.05 steps.
- Medium on the Radeon against the NVIDIA card: 0.012 steps on average, as
  before.
- What a traced frame costs, at the game's own pace on the stage that
  costs most (22), three runs of each, in ms, release 0.5.23 and now:

  | | High | Medium | Low |
  |---|---|---|---|
  | NVIDIA, 0.5.23 | 11.5 to 12.1 | 7.9 to 8.9 | 7.5 to 8.2 |
  | NVIDIA, now | 11.5 to 12.2 | 7.8 to 8.5 | 7.7 to 8.1 |
  | Radeon, 0.5.23 | 18.6 to 20.1 | 8.1 to 10.0 | 4.9 to 8.1 |
  | Radeon, now | 18.1 to 18.8 | 8.9 to 11.1 | 6.0 to 8.9 |

  The same within what runs differ by. The Radeon's two lighter levels
  swing by a third from one run to the next in both builds (a graphics
  chip that is not kept busy runs slower), so their rows say no more than
  that.
- The Windows game under Proton, one stage: its Vulkan picture is the
  Linux game's to the pixel at the three levels. DirectX 12 against
  Vulkan: 4,161 to 7,891 of 1,228,800 pixels differ, by 3 steps at most
  (they were a few hundred: the two compilers round the rays' angles
  differently, and a ray that grazes an edge falls on either side).
- Both development builds compile with no warning.

**Known.**

- The pattern still stays put on the screen. It is fainter, not gone.
- A pixel in a soft edge whose first rays happen to agree takes no more:
  the edge's outermost part is a little harder than the reference's.
- Low gained next to nothing.
- Nobody but the maintainer can say what it looks like in motion.

**Not covered.** Windows itself; a wide picture and other internal
resolutions (the rays' pattern goes by the traced pixel, whatever the
size); a two-PC game (the tracer reads the frame, it changes nothing of
the game).

## 81. Traced water (2026-11-25)

**Why.** With ray tracing on, panes of glass mirror the scene by rays
(section 74), and the water of three stages did not: it showed the game's
own reflection picture (section 78), which the processor drew, because the
ray tracing's device had no such picture. The maintainer asked whether the
water could be ray traced too: "Outside there would be more reflection in
the water than inside a cave of course."

**What the game does, looked at first** (temporary probes, gone again):

- The water is model strips in the opaque list, grid tiles of 1.5 units a
  cell, with the reflection pass's picture as their texture, drawn as it is
  (source blend one, no blend with what is behind), times the vertex
  colors: white on stages 19 and 20, a brown on the Shrine (stage 21),
  where a color is added too.
- The grid is not flat: heights up to about a tenth of a unit, which the
  game moves on stages 20 and 21. The strips have no normals.
- The sky and the backdrop of all three stages are strips a ray can hit:
  with a marker color for rays that leave the scene, no pixel showed it.
- **The reflection pass is not the scene mirrored.** It draws models of
  its own: 229 strips against the screen's 1,140 on stage 19, 27 against
  527 on stage 20, all without lights; none shares its vertices with the
  screen's. On stage 19 the picture is near-black with a few reflections,
  on stage 20 a teal of the game's own making. Only the Shrine's (207
  strips against 386, mostly lit scenery) is close to a mirror of what is
  there, in an orange haze: the pass has the game's fog.

So a mirror by rays in the picture's place changes two of the three
stages' look a great deal. The maintainer saw both ways side by side and
chose: the true mirror on the Shrine, the mirror over the game's own
picture on the others.

**What is done** (Vulkan ray tracing; a helper session built the first
version in a worktree of its own, and it was merged by hand):

- *Which polygons.* The game's own rule: a triangle of the opaque list
  whose state takes a reflection pass's picture (section 78's `target`)
  is water (`RT_TRI_MIRROR`). No guess from the geometry.
- *The mirror.* For a pixel whose eye ray meets water, a ray goes on
  mirrored in the water's plane: the mean plane of all the water's
  triangles (`rt_scene_fill`), with 15 % of the grid's own height over it,
  so that the game's swell is there without swinging the picture. The
  water itself is not in that ray's way. What it meets is shaded as a
  bounce is (its color times the game's lights), with the pass's fog by
  the mirrored point's depth; a ray that leaves the scene shows the pass's
  own background color. The result goes out with the glass's reflections,
  and the renderer puts it in as it does those.
- *In the picture's place (the Shrine).* The mirror takes the picture's
  whole share, times the polygon's own colors as the picture would be. The
  reflection pass is then neither drawn nor handed on at all
  (`view_host_rt_mirror`): the view marks the states as it does for a pass
  the graphics chip draws, and says so in the frame (`rt_mirror`).
- *What is under the mirror there.* The picture the game's last pass left
  is still the water's texture, and it showed: where the mirror's share is
  not whole, which is along a waterline at Medium (of the traced pixels
  around a picture pixel some are water and some are bank, as far away as
  each other, and the fill takes both). What it held depended on how many
  of the game's passes had been drawn before the mirror took over, one or
  two, which is a matter of timing: one run in four drew an orange line a
  pixel or two thick along the far bank and round the posts, the others
  none (found 2026-11-25 by running one case four times: two pictures,
  1,785 pixels apart). Proved by clearing that picture to a marker color:
  the line took the color, in every run. So the water of a frame that is
  traced with the mirror in the picture's place is drawn over a black
  picture of the same size (`mirror_under`, `t_under`): nothing of the
  game's picture is under the mirror. The game's own picture is left as it
  is, for the frames that still come without a pass once ray tracing is
  switched off; one that was never drawn is made and cleared, so that the
  states can name it (`mirror_rooms`).
- *Over the picture (the other stages).* The graphics chip draws the pass
  as without ray tracing (section 78; before this the processor drew it
  whenever ray tracing was on), and the mirror goes over it by the share
  water has at the eye's angle: 2 % looking straight down, all of it along
  the surface. Looking down the game's picture stays; along the water the
  scene shows in it.
- *Which stage.* The game window asks the game which stage it is on
  (`game_machine_stage`, the match record's stage) and tells the renderer
  (`gpu_render_water`): 1 on stage 21, 2 elsewhere. The renderer takes it
  up with a frame.
- *What else the water shows.* A bounce ray or a pane's mirror ray that
  meets water takes the scene mirrored once in the plane, without fog
  (`water_seen`): the water's own texture is not there to read.
- DirectX 12 ray tracing is as before: the processor draws the pass and
  the water shows it.

**Checked** (the development PC, the Linux game from stage snapshots in a
headless desktop, 1280 x 960, an RTX 5070 Ti):

- The Shrine's intro, where the pond is in view: bridge, rocks, trees and
  sky are mirrored where the game's own reflection has them, sharper; the
  shrine and the far bank, which the game's pass leaves out, are in it. No
  pass is drawn after the first two frames.
- Stages 19 and 20: the game's water with the scene coming into it along
  the surface; 176 and 184 passes drawn by the graphics chip in as many
  frames.
- A stage without water with ray tracing on: the same to the pixel as
  before this section (section 80's pictures), Medium and High.
- Ray tracing off: the same to the pixel as release 0.5.23 on the three
  stages.
- The Shrine once the mirror stands, run after run: four runs at Medium
  (one of them had drawn two of the game's passes first, the others one),
  two at High: one picture each, the one without the line. Low runs. The
  other stages are the same to the pixel as before that change (stages 22,
  3, 19 and 20 at Medium, High and without ray tracing), and so is the
  Shrine without ray tracing.
- The Windows game under Proton: on stage 19 at Medium and on the Shrine
  at Medium and High through Vulkan the same to the pixel as the Linux
  game (its Shrine runs had drawn two passes first), and so without ray
  tracing; through DirectX 12 it runs and shows the game's own water, as
  meant. Stage 3 through both is as section 80 left it.
- What a traced frame costs did not change by more than runs differ. On
  stage 19 the machine's thread no longer draws the pass with ray tracing
  on: about 4.3 against 5.8 ms a frame (the helper's figure, taken while
  other tests ran).

**Known.**

- The mirror is flat but for that part of the swell: the strips have no
  normals to ripple it by, and the triangles' own tilt would show as
  facets.
- One plane for all the water of a scene. Water at two levels would be
  wrong; no stage has it.
- The Shrine's traced water is clearer than the game's hazy one: only the
  fog table's fog is on the mirrored points.
- The first frame or two of a stage show the mirror over the game's
  picture, as on the other stages, before the Shrine's own way is taken
  up; and the frame or two after ray tracing is switched off show the
  game's reflection as its last pass left it.
- On the two blended stages the water's edge against the floor above it is
  smoothed with its neighbors as any reflection is.
- Along a waterline on the Shrine the mirror is dimmed over a pixel or two
  at Medium: its share there is not whole, and what is under it is black.
  The bank beside it takes a little of the mirror, as beside any glass.

**Not covered.** The Radeon, Low, a wide picture, Frame skip, a two-PC
game, switching ray tracing or its quality in the menu on a water stage,
stages 18 and 15, the Windows game on stage 20, real Windows.

## 82. The traced light averaged over frames: tried, not kept (2026-11-25)

**Why it was tried.** After section 80 the grain that is left comes from
the few rays a pixel has. Path tracers of other games get rid of it with
three things, and the maintainer asked for them: each pixel's light kept
and averaged over frames, bounce light kept in the scene, and filters
that compare the surfaces' normals. The first was built whole, the third
with it; the second was not begun.

**What was built** (Vulkan; none of it is in the tree any more):

- Each traced pixel found where its surface point was in the frame before,
  took the mean kept there if that was still the same surface (as far from
  the eye, facing the same way), and put the frame's value into it: a
  third pass between the tracing and the smoothing, means of up to 16
  frames, the rays different from frame to frame.
- A test for a light that has changed (a shadow that has moved on), which
  drops the mean: each neighbor's value of the frame against what was kept
  for it, their mean against its own uncertainty.
- The smoothing pass skipped neighbors that face another way.

**What it took, and what is worth knowing for anything that follows a
surface from frame to frame:**

- *A strip's place in the game's memory is no name for it.* About half the
  strips of a frame, the fighters' among them, lie in lists the game
  writes anew every frame, at another place each time: by place 336 of
  747 strips were found again on one stage. A sum over what a pose does
  not change (the strip's length, its format, its vertices' strip marks
  and texture coordinates), with the strip's turn among those of one sum,
  found 739 of 747, and 1,035 of 1,035 on another stage.
- *A strip's matrix need not say how its points moved.* Many strips share
  one matrix (on one stage 444 of 1,035 had the camera's alone), which
  fits vertices the game has worked out itself, frame by frame; that was
  not looked into further. Each triangle's own corners of the frame
  before follow any motion and need no matrix, and that is what was used.
- *Rays that change from frame to frame must change in two ways at once.*
  Moving the pixel's place in the noise (section 80's provision) moves
  both of its numbers by one step: the rays of all frames lie on a few
  curves, and their mean is of those curves, not of the disk. Two steps
  that share no rhythm cover the disk. (Right by the arithmetic; it did
  not change the figures below, where the trailing outweighs it.) And the
  mirrors' rays must stay as they are: their pictures are not averaged,
  and would flicker.
- *A pixel's own test for change did not help.* Alone it let most of the
  trailing through (error 6.1 against 1.8 with the neighborhood's test, in
  an attack); with the other it added flicker (0.52 against 0.49,
  standing). Its likely fault: one unlucky dark value looks like a change
  and the pixel's mean is dropped.
- *A shadow goes with what casts it.* A floor's kept shadow was taken from
  where the fighter in the light's way had been (what the shadow rays met,
  moved back, along the floor). It helped a little (error in an attack
  2.4 to 2.1).

**Measured** (Medium, 1280 x 960, an RTX 5070 Ti, the game at its own
pace from stage snapshots, five frames in a row, each against a picture of
the same frame traced with 48 rays; error: how far from that picture;
flicker: how much the picture changes from frame to frame beyond what that
picture changes; both RMS in steps of 255):

| | every frame by itself | | averaged | |
|---|---|---|---|---|
| | error | flicker | error | flicker |
| standing, stage 22 | 0.83 | 0.67 | 1.05 | 0.49 |
| standing, stage 3 | 0.41 | 0.36 | 0.87 | 0.53 |
| attacking, stage 22 | 0.81 | 1.06 | 2.11 | 1.92 |
| attacking, stage 3 | 0.42 | 0.58 | 1.38 | 1.59 |

A stricter test for change brought the averaged figures nearer to the
others (standing 0.91 / 0.49 and 0.68 / 0.48, attacking 1.39 / 1.49 and
0.95 / 1.17) and never past them, but for the flicker of one stage
standing. A traced frame took 1.0 to 1.2 ms longer (about 14 %); where
that went was not measured (the processor writing each triangle's old
corners is the likely part).

**Why it does not pay here.**

- Nothing stands still. In their stance the fighters bob by a fraction of
  a pixel every frame, and their shadows on the floor and on themselves
  move with them. A mean of several frames trails behind a shadow that
  moves; in an attack it smears it. The test for change can only take the
  mean away again, which leaves the frame by itself, with rays that now
  differ every frame.
- Little is left to win. Section 80 took a quarter to a half of the grain,
  and a whole frame is 0.4 to 0.8 steps from the many-ray picture. Its ray
  pattern stands still on the screen, so what grain there is hardly moves;
  a mean of a few frames is smoother but shimmers.
- The normals' test took neighbors away where there are few (toes,
  fingers), which left speckle there.

So the frame by itself, as section 80 left it, is the better picture in a
fight, and all of this was taken out again. Checked after that: the
pictures of sections 80 and 81 to the pixel (stages 22, 3, 19 and 20 at
Medium, High and without ray tracing; the Shrine in both of the pictures
its start then gave, which section 81 has since made one).

**If it is taken up again.** The only shape that cannot do harm: average
only where nothing has moved (the pixel's own place and its shadow's
caster within a fraction of a pixel, the neighborhood's test passed), and
let a pixel's rays change from frame to frame only while its mean is
alive; everywhere else exactly section 80. That would clean the picture in
a pause, a win pose or an intro, and nowhere else. The old corners belong
on the graphics chip (last frame's own buffer and one index a triangle),
not written by the processor.

## 83. A watch on the window's thread (2026-11-26)

**Why.** Issue 3, after frame skip was put right for its reporter: on his
PC (Intel HD Graphics 620, Windows, a television as its screen) the
picture sometimes freezes in a fight while the sound goes on. His log
shows it: within ten seconds the window's presents stop, and the machine's
lines go on as before, with no error. Asked, he says nothing brings the
picture back (the menu's key, Alt+Enter; Alt+Tab reaches the desktop only
after many tries), that it has happened in full screen only, and that the
PC is on mains power. Here it has never happened: three runs of twenty
minutes with his settings (the Windows game under Proton on one graphics
chip, the Linux game on two; 34 matches) showed every ten seconds with
their full count of presents.

So the window's thread does not come back from one of its steps, and the
log did not say which. This section makes it say; it does not make the
freeze go away, whose cause is not known.

**What is done** (`runtime/host/main.c`):

- The window's thread marks each step of its loop (`win_at`): between
  frames (events, the menu), starting the renderer again, waiting for the
  screen's next buffer (`SDL_WaitAndAcquireGPUSwapchainTexture`), drawing
  the frame, handing the frame to the graphics driver
  (`SDL_SubmitGPUCommandBuffer`), after the frame, ending, closing the
  window.
- A thread of its own (`win_watch`) looks four times a second. When the
  step has stood for two seconds it writes a line: the step, the turn of
  the loop, the presents so far, what the window was at the turn's start
  (fullscreen, minimized, covered, in front) and the machine's vertical
  blank and whether it runs. It writes again at 10 and 30 seconds and then
  every minute, and once more when the thread goes on, with how long it
  stood. It only reports: a thread cannot be got out of a driver's call,
  and the watch asks nothing of SDL or of the window.
  Between looks it waits on a semaphore, which the closing gives: the
  game does not wait a quarter of a second for it at its end (a first
  version slept the time out, and the release check's quit under Proton
  took 0.29 s where it had taken 0.10).
- The log now has the window's events it lacked: in front, no longer in
  front, minimized, covered by others, and a display whose mode changed.
- And the graphics driver's name, version and description as SDL has them
  (`graphics driver:`): the log had only "Intel driver".
- `--stall-window T:MS` holds the window's thread for a test, where it
  waits for the screen's next buffer.

**Reading its lines.** A line of the watch is not always a fault. A
renderer that starts again, a window that is slow to close (section 60's
driver), a PC that slept, and on Windows a window held by its title bar
(the system's own loop may keep the thread) can each stand for more than
two seconds; the step's name tells them apart, and each ends with a "went
on" line. The freeze of issue 3 would be "waiting for the screen's next
buffer" or "handing the frame to the graphics driver" with no such line
after it.

**Checked** (the dev build at the game's own pace in a headless desktop,
stage 22 from its snapshot):

- Held for 5 s: the line after 2 s, naming the wait for the screen's next
  buffer, turn 120, the window in front and not fullscreen, the machine
  running; then "went on after 5.2 s". Held for 12.5 s: also the line at
  10 s.
- Not held: no line of the watch.
- The picture at the end is the same to the pixel in all three as before
  this section.
- The Windows game under Proton, held for 5 s: the same two lines, the
  driver's line, and the Linux game's picture to the pixel.

**Not covered.** A thread that really does not come back (the lines while
it lasts are checked, the end is the same code); Windows itself; the
reporter's PC, where the freeze has to happen once more with this build
for the log to say where.

## 84. The mouse wheel in the launcher (2026-11-26)

**Why.** The maintainer noticed that the scroll wheel does not scroll in
the launcher. Its Settings have lists longer than their panel: the Picture
tab, and the two controls tabs.

**The cause.** The game's menu takes the wheel (a turn moves the choice,
`menu.c`); the launcher never did. Its router handed the mouse's moves and
clicks to the page's mouse handling and everything else to the keys'
handlers, which drop what is not a key or a button. Shown by a test before
anything was changed: three steps of the wheel left the Picture tab and
the Gamepad tab as they were, where three presses of Down moved the
choice.

**What is done** (`runtime/launcher/launcher.c`):

- `wheel_event`: a step of the wheel moves the choice of the list on show
  one row, as in the game's menu, and the list scrolls with its choice:
  Settings' rows, a controls tab's, the cards, the card editor's. It does
  not wrap as the keys do: a wheel turned on stops at the list's end.
  Nothing while a note is up or a controls cell waits for its key.
- The steps are SDL's whole ones (`integer_y`), so a touchpad's many small
  moves add up to a row and are not a row each.
- `--shot-keys` takes `wheel:up` and `wheel:down`, for the test.

**Checked** (the Linux launcher in a headless desktop, on the Picture tab
and the Gamepad tab): three steps down give the picture of three presses
of Down, to the pixel; 40 steps and 41 give one picture, the list's end;
40 down and 40 up, and a step up at the start, give the page as it was.
The Cards tab without cards takes a step and nothing happens. The Windows
launcher builds.

**Known.**

- The game's menu takes one row for every wheel event, whatever its size:
  on a touchpad its choice moves fast. Not changed here.
- These test pictures are drawn by SDL's software renderer
  (`SDL_RENDER_DRIVER=software`): with SDL's OpenGL renderer, which a test
  picture uses otherwise, the Picture tab's picture comes out black on the
  development PC, perhaps because that tab asks Vulkan for the graphics
  chips. Section 79's check of the launcher's page had such a black
  picture and rested on the file the launcher saved. It was made again
  with the software renderer: for a settings file of an earlier layout
  that says `cpu` the Lighting row shows GPU, and for one of layout 3,
  CPU.

**Not covered.** A real wheel turned by hand (the events were scripted),
the Windows launcher run, a Cards tab with cards, the card editor.

## 85. The wait for the screen's next buffer, in two steps (2026-11-26)

**Why.** Section 83's watch did what it was made for. The reporter of
issue 3 ran 0.5.24, in full screen from the start this time, and the
picture froze again. His log says where: the window's thread stood
"waiting for the screen's next buffer
(`SDL_WaitAndAcquireGPUSwapchainTexture`)", the window in full screen and
in front, the machine running. After 25.6 seconds it went on, at the
moment the window lost the keyboard; it put one more picture on the
screen and stood in the same call again, to the log's end.

Beside the log before it, the two stops look like this:

| | 0.5.23's log | 0.5.24's log |
|---|---|---|
| Full screen from | 3.9 s (Alt+Enter) | 0.6 s (the setting) |
| The picture stopped at | 332 s | 337 s |
| Presents by then | 19,890 | 20,180 |
| The game was | in the middle of a round | at the attract mode's first logo |

So it comes about five and a half minutes, or about 20,000 presents, into
full screen, whatever the game is drawing. What happens then is not
known, and this section does not make the freeze go away. It makes the
next log say more.

**What the call holds.** SDL's Vulkan code (SDL 3.4.20,
`VULKAN_INTERNAL_AcquireSwapchainTexture`) waits twice in that one call,
each time without an end: first for the frame handed over two presents
before to be finished by the graphics chip (SDL keeps two frames in
flight, a fence for each), then in `vkAcquireNextImageKHR` for the driver
to give the next of the swapchain's images. The first is the chip not
finishing a frame; the second is the screen's side not giving a buffer
back. One step in the watch could not tell them apart.

**What is done.**

- `runtime/host/main.c`: the call is made as its two parts,
  `SDL_WaitForGPUSwapchain` (the first wait) and
  `SDL_AcquireGPUSwapchainTexture` (which finds the earlier frame
  finished and goes on to the driver). Each is a step the watch names:
  "waiting for the graphics chip to finish an earlier frame" and "waiting
  for the screen's next buffer". The waits and their order are SDL's own,
  as before.
- `runtime/host/sysidle.c`: on Windows the watch's lines end with how
  long the system has seen no key and no mouse (`GetLastInputInfo`).
  Windows keeps that time for its own idle timers, and the reporter plays
  with a gamepad: in the first log his last key was Alt+Enter, 328
  seconds before the stop, and the second log has no key at all. That is
  a guess to be tested and no more. SDL asks Windows to keep the display
  on, and he sees a frozen picture, not a dark screen.
- The log's start has what Windows does with a PC left alone (`this PC
  left alone:`): mains or battery, the screen saver and its wait, the
  power plan's time to switch the display off (read through
  `powrprof.dll`, loaded for it), and whether the game keeps them away.
  It does: SDL asks for that by default when its video starts
  (`SDL_DisableScreenSaver`, which on Windows is
  `SetThreadExecutionState` with `ES_DISPLAY_REQUIRED` on the window's
  own thread), and the line says what SDL answers.
- `--stall-window T:MS:1` holds the first of the two steps for a test
  (without the `:1`, the second, as before).

**A second opinion, looked into.** The maintainer passed on an analysis
that reads the time as Windows' five-minute idle timers and names four
things to do. Against the code and the two logs:

- *Keep the screen saver and display sleep away through SDL.* Done
  already, by SDL's default (above). The new line shows whether it holds
  on the reporter's PC and what his timers are.
- *Intel's panel self refresh; another present mode.* Panel self refresh
  is the laptop's own panel's, and he plays on a television with the
  laptop's screen off. The game always presents with vertical sync
  (SDL's default); another mode would be a trial, not made here.
- *A counter wrapping or a store running out at some frame count.* The
  two stops are not at one count (19,890 and 20,180 presents), and
  neither is near a switch to full screen (3.9 s into one run, the
  setting in the other). A graphics driver's reset in Windows' own
  record (Event Viewer, event 4101) at that time would say much: asked
  of the reporter.
- *An end to the wait, and a new swapchain when it passes.* SDL's Vulkan
  code gives `vkAcquireNextImageKHR` no end and has no setting for one,
  so this needs SDL's own source changed and a build of it shipped. Left
  until the next log says which wait it is.

**The full screen itself.** The maintainer asked whether a windowed full
screen could be made as a way round. The game's full screen is one
already: it never sets a display mode, and SDL on Windows
(`WIN_SetWindowFullscreen`) makes the window a borderless one on the
monitor's own rectangle. SDL's Vulkan code neither asks for nor forbids
the driver's exclusive full screen, so what a driver does with a window
that covers the screen exactly is the driver's choice. A borderless
window that is not the screen's exact size (a row taller, which other
programs have used to stay an ordinary window to the system) was looked
at and turned down: it makes no sense as a mode of a game. What is to
follow is an exclusive full screen as a mode of its own, with the present
one named for what it is, a borderless window.

**Checked** (stage 22 from its snapshot at the game's own pace, in a
headless desktop):

- The Linux game held 5 s in the second step: the watch names the
  screen's next buffer (`SDL_AcquireGPUSwapchainTexture`), and "went on
  after 5.2 s". Held in the first step: it names the earlier frame
  (`SDL_WaitForGPUSwapchain`), and "went on after 5.0 s". Not held: no
  line of the watch, 607 presents in the ten seconds and the same line of
  the pace as before the change.
- The picture at the end is section 80's to the pixel in all three.
- The Windows game under Proton, held in each step: the same lines, each
  ending with the system's idle time, and the Linux game's picture to the
  pixel.
- The Windows game with SDL's Direct3D 12 driver (`--gpu d3d12`, the
  software renderer's picture): held and gone on the same way, 60
  presents a second after it, a clean exit.
- The line about the PC left alone, under Proton: "on mains; no screen
  saver set; the display's switching off not known; kept away by the
  game while it runs (SDL): yes". Wine does not give a power plan's
  values. The Linux game writes no such line.

**Not covered.** Windows itself (the idle time under Wine is Wine's, and
the power plan's values have not been read anywhere yet);
the reporter's PC, where the freeze has to happen once more with this
build for the log to say which of the two waits it is; a thread that
really never comes back.

## 86. Fullscreen's two kinds: borderless window and exclusive (2026-11-26)

**Why.** Issue 3's freeze comes in full screen only, and section 85 found
what the game's full screen is: a borderless window on the monitor's own
rectangle, with no display mode set. The maintainer decided three things.
That kind is named for what it is, Borderless window. An exclusive full
screen is added as a kind of its own. And the launcher never goes
exclusive, so that a setting that breaks things can always be changed
back there.

**What exclusive is here.** SDL's: the game hands SDL a display mode
(`SDL_SetWindowFullscreenMode`), and SDL sets the display to it while the
window is fullscreen and has the keyboard, minimizes the window when
another takes over, and gives the display its own mode back then and when
the game leaves fullscreen or ends. It is not a promise about how the
graphics driver presents. SDL's Vulkan code neither asks for nor forbids
a driver's own exclusive presentation (section 85), and asking would mean
changing SDL's source and shipping a build of it. So for issue 3 this is
one more thing its reporter can try, not a known cure.

What it gives every player is the screen at a mode of his choosing. The
game shows 60 frames a second, and a screen that runs at 144 Hz shows
them unevenly (section 62's notice); set to 120 or 60 Hz for the game, it
shows them evenly.

**What is done.**

- Settings (`settings.c`): `fullscreen` is `auto`, `off`, `borderless` or
  `exclusive`. `on`, which older files have, is read as `borderless`.
  `screenmode` is `auto` (the desktop's own mode) or a mode as text,
  `WIDTHxHEIGHT@HZ` (`1920x1080@60`, `1280x720@59.94`).
- The rows (`options.c`), in the launcher and in the game's menu:
  Fullscreen goes through Auto, Off, Borderless window, Exclusive. Under
  it, Screen mode goes through Auto and the modes of the chosen display
  (`devices.c`: `screen_modes_list`, from SDL's list, one of a kind, none
  under 640 x 480); it is grayed unless Fullscreen is Exclusive.
- The game (`main.c`, `fullscreen_kind`): before the window goes
  fullscreen, or in place when it is, Exclusive gives SDL the display's
  nearest mode to Screen mode (`screen_mode_find`; the desktop's own for
  Auto and for a mode the display has nothing near), and anything else
  gives it none. A mode SDL does not take ends as the borderless window,
  and the log says why. It is done as the window is made, on F11 and
  Alt+Enter (which switch between the window and the kind the setting
  names), when the menu changes either row, and when Display moves a
  fullscreen window to another display (the old display's mode is let go
  first).
- The log: the settings line says `fullscreen off`, `borderless` or
  `exclusive` and the screen mode, and each switch to Exclusive says the
  mode it took and on which display.
- The launcher (`launcher.c`): its own window follows the game's
  Fullscreen as before, as a borderless window for either kind. It lists
  the display's modes for the row and never hands SDL one: the file has no
  call of `SDL_SetWindowFullscreenMode`, and the checks count that. A
  note under the Screen mode row says so.

**The way back from a mode that fails.** Alt+Enter or F11 leaves
fullscreen, Alt+Tab minimizes the window, and closing the game (Alt+F4)
ends it: SDL gives the display its mode back in each case. Fullscreen is
then set back in the launcher. Section 87 adds the question that takes a
mode back by itself after some seconds.

**Two faults found by looking at the pictures.** The launcher's page kept
its rows in buffers for 14, and the Picture tab now has 15: the first
row's value came out as garbage. They hold 16 now, by one constant with
the table of rows. And the two texts with a note under them ran over the
foot of the launcher's panel, which is smaller than the menu's: the texts
are shorter, and the launcher's note stays with Screen mode alone.

**Checked** (the development builds, each in a headless desktop, so that
no real screen's mode is touched; stage 22 from its snapshot):

- The Linux game: a window; `borderless`, and `on` as older files say it;
  Exclusive with Screen mode Auto (1920 x 1080 at 60 Hz, the desktop's
  own); Exclusive at 1280x720@60 (the swapchain is 1280 x 720); a mode no
  display has (9000x9000@60: the desktop's own); a line that is no mode
  (refused with the settings' usual message, Auto). All run to their end,
  and the game's picture at the end is the window's to the pixel in each.
- F11 out of Exclusive and back: the window, then the same mode again.
- In the menu, from a window: Fullscreen to Borderless window, to
  Exclusive, then Screen mode one step. Each took at once, and the
  settings file afterwards has `fullscreen = exclusive` and the mode.
- The launcher's Picture page, as pictures: both rows with their values,
  Screen mode grayed for a window, the notes inside the panel.
- The Windows game under Proton: a window, Borderless window, Exclusive
  at the desktop's own mode and at 1280x720@60. There the display's mode
  did change ("Display1 is now at 1280 x 720, 60.00 Hz"), the swapchain is
  1280 x 720, the game ends cleanly, and the picture is the Linux game's
  to the pixel in all four.

**Known.**

- On a Linux desktop with Wayland a program cannot set the screen's mode:
  SDL draws the picture at the mode's size and the desktop scales it, and
  the refresh rate stays the screen's. The row's text says so.
- A settings file written by this build says `borderless` where older
  builds wrote `on`. An older build that reads it does not know the word
  and takes its default (Auto).
- Stepping through the Fullscreen row in the game's menu passes
  Exclusive on its way round, and the display's mode changes at that
  step when Screen mode is not the desktop's.

**Not covered.** Windows itself, and a real screen's mode really set: the
checks ran on a headless desktop's virtual screen (Wayland's scaling for
the Linux game, Wine's own mode setting on that screen for the Windows
game). Two displays. A Steam Deck. The window minimized when another
takes over. The Windows launcher's page (the Linux launcher's was looked
at; the code is the same). The reporter's PC.

## 87. "Keep this screen mode?" (2026-11-26)

**Why.** Section 86 left a player whose screen shows nothing at an
exclusive mode with Alt+Enter, Alt+F4 and the launcher, which he has to
know of. The maintainer asked for what other programs do: a question
that undoes the change when nobody answers.

**What is done.**

- When Fullscreen Exclusive takes a Screen mode the player has not kept
  before, the game asks "Keep this screen mode?", naming the mode, with
  Keep and Go back and the seconds left (15). It starts on Go back, so
  that a key pressed blind does not keep a mode that shows nothing.
  Keep remembers the mode in the settings (`screenkept`), and it is not
  asked about again. Go back, Esc, a pad's B, or no answer in time puts
  Fullscreen and Screen mode back as they were, in the settings file too:
  the next start does not take the mode again.
- "As they were" is what the menu's rows had before the change, when the
  change was made in the menu. At the game's start (the launcher chose
  the mode) and on F11 it is the borderless window, with the mode kept
  last.
- Screen mode Auto, the desktop's own mode, is never asked about.
- The question is one of the menu's notices (`menu.c`,
  `MENU_NOTICE_MODE`). Asked while the menu is open, it lies over the
  page and gives the page back after the answer; at the start it opens
  the menu for itself and closes it. The game is held while it shows, as
  for the menu. The window's loop counts the seconds (`main.c`,
  `mode_ask_turn`).
- Leaving fullscreen while it asks (F11) is taken as Go back, and the
  window stays a window.
- `--mode-ask N` gives the question N seconds, for the tests.

**A fault found by the first run.** `--mode-ask` read its number through
`SDL_max`, a macro that names its argument twice, and so stepped over two
command-line words: the next option was taken for an unknown one. It
reads the number first now.

**Checked** (the development builds in a headless desktop; Exclusive at
1280x720@60, a mode not kept before, unless said):

- At the Linux game's start the question comes up, naming 1280 x 720,
  60 Hz (its picture was looked at). Left and Enter keep it: the settings
  then have `screenkept = 1280x720@60`, and the game goes on at that mode.
- Enter on the choice it starts on, and Esc: back to the borderless
  window, and the settings have `fullscreen = borderless` and Screen mode
  Auto. No answer in 3 seconds (`--mode-ask 3`): the same, 3.0 s after it
  asked. F11 while it asks: taken as Go back, and the window stays a
  window.
- A mode the settings say was kept, and Screen mode Auto: no question.
- In the menu, from a window: Fullscreen to Exclusive (no question: its
  mode is the desktop's own), then Screen mode one step: the question
  over the menu. Keep: the menu's page again, the mode kept in the
  settings. Go back: the page again with Screen mode at Auto and
  Fullscreen still Exclusive, as before the step (its picture was looked
  at).
- A mode no display has (9000x9000@60), not answered in 2 seconds: back
  to the borderless window.
- The game's picture at the end is a window's to the pixel in every one
  of these.
- The Windows game under Proton: the question at the start, kept by the
  same keys; and not answered in 3 seconds, where the display went to
  1280 x 720 and back to 1920 x 1080 and the game went on as the
  borderless window to a clean exit. The picture is the Linux game's to
  the pixel in both.
- Section 86's cases once more with this build (ten with the Linux game,
  the launcher's page, four under Proton): as they were.

**Known.**

- The question's seconds are not shown counting in any picture taken:
  the log has the time it gave and what happened at its end.
- A mode the display does not have is asked about under the name of the
  nearest mode the display took for it.
- Stepping through the Fullscreen row past Exclusive with a Screen mode
  not kept brings the question up on the way.

**Not covered.** Windows itself and a real screen, as section 86. A
gamepad's buttons on the question (the keys and the menu's commands are
the same ones the rest of the menu takes). The mouse on its buttons. A
screen that really shows nothing.

## 88. The frame rate display (2026-11-26)

**Why.** The maintainer asked for a counter of frames a second and of the
frame's time, with a small graph, after noticing that ray tracing at a
high Internal resolution lowers the frame rate. The game measured all of
it already and wrote it to the log only, every ten seconds.

**What it shows.** A panel at the window's bottom left, while the game
runs:

- The pictures shown a second, and the time from one to the next, over
  the last half second (30 pictures of a 60 a second game).
- What the game's machine took for its last frame ("game"), and with ray
  tracing on what the trace of the last frame took, with its way back
  from the graphics chip ("rays"), both smoothed.
- The slowest picture among the last 120.
- With **Numbers and graph**, a bar for each of the last 120 pictures,
  the newest at the right. The graph is two frames high (33.4 ms), a line
  marks one frame at 60 a second (16.7 ms), and a bar over 20 ms is in
  the accent color.

A picture is one the game drew and the window showed for the first time:
with Frame skip at Every other frame that is 30 a second. A gap of a
second or more (the menu, a loading) is not counted, and the count starts
anew after it.

**What is done.**

- `runtime/host/perf.c` (new): keeps the last 120 times, and draws the
  panel with the menu's own drawing kit. The numbers are written anew
  four times a second, so that they can be read; the graph with every
  picture.
- `main.c`: the window's loop tells it of each new picture, by either
  renderer. The panel is drawn at the top left of the menu's surface,
  which the menu, the operator bar and the note have handed over by then,
  and goes to a small texture of its own; `compose` puts that over the
  picture. Not while the menu is up. A picture taken of the window
  (`--keys shot=`) has it; the game's own pictures (`--shot`) do not.
- The machine's thread leaves its last frame's time for it, and
  `gpu_render.c` the last trace's.
- The setting is `stats` (`off`, `numbers`, `graph`), the row **Frame
  rate display** at the end of the Picture tab, in the launcher and in
  the game's menu, where it takes at once. Off by default.

**Where it sits.** It was at the top left first, and lay over player 1's
life bar when the picture fills the window. At the bottom left a fight
shows nothing; with a 4:3 picture in a wide window it is in the black
side.

**Checked** (the development builds in a headless desktop at the game's
own pace, stage 22 from its snapshot; the window as shown, taken at
vertical blank 7300):

- Off: the window has no panel.
- Numbers: the panel at the bottom left reads 59.9 fps and 16.7 ms, the
  game's time (5 to 7 ms there) and the slowest of the last 120. Numbers
  and graph: the same over its bars, a level band at one frame's height.
- With ray tracing on, "rays" is in the second line: about 9.5 ms, where
  the log's average over the run is 8.2 to 8.4 ms.
- A 16:9 picture in the borderless fullscreen at 1920 x 1080: the panel
  is clear of the life bars and of player 1's card signs (the pictures
  were looked at; a first, wider panel lay over those signs).
- The software renderer: the panel shows the same.
- From the menu: the row at the Picture tab's end, set to Numbers and
  graph; the panel shows once the menu has closed, and the settings file
  has `stats = graph`. The launcher's page has the row and its text.
- With the display on, the game's own picture at the end is the same to
  the pixel as without it, and the presents in ten seconds are as many
  (604 to 607).
- The Windows game under Proton, with the graph and ray tracing: the
  same panel, 604 presents in its ten seconds, a clean exit.

**Known.**

- The panel is opaque and has one place. On the title screens and in the
  test menu it may lie over the game's own writing.
- "game" is the time the machine's thread worked for a frame, not the
  whole frame's: the window's own drawing, the wait for the screen and
  the graphics chip's work are in the time from picture to picture only.
- There is no key for it; it is switched in the menu.

**Not covered.** Windows itself. A screen that is not at 60 Hz, a slow
PC, and Frame skip, where the numbers matter most: the headless desktop
gave a steady 60. The numbers were read from pictures, not compared with
another counter.

**A fault found by the next release's check (2026-11-27).** The display's
row is the Picture tab's sixteenth, and the game's menu kept a tab's rows
in two arrays of 15: `ACT_COUNT`, the controls' count, taken for the
rows' when the controls came into the menu (section 72). The sixteenth
row was written past both. The development builds and the Windows game
went on by luck; the Linux game that the release's own builder makes
fell over as the Picture tab was drawn (in `ui_text_in`, a row's text no
pointer). The arrays are `ROWS` now, the size of the table the rows come
from, so a tab cannot have more rows than they hold. Release 0.5.26's
check of its fresh zip found it, before anything was published.

## 89. A spot light's cone, for a light that goes to the color (2026-11-26)

**Why.** Players say some stages' lighting looks flat and their colors
muted. The rooftop, which they pointed to, turned out to be as the game
draws it: a traced run (the headless game on its interpreter, the object
and light routines logged) shows a daylight stage by the game's own
numbers, with four helicopters, the red lights on the fence and no beam,
and the picture has all of it. The dusk rooftop in their screenshots is
another version of the game.

A reading of the decompilation beside the renderer found a gap that is
real, on other stages. Section 64 worked out a spot light's cone for the
one light that goes to the alpha, and left the lights that go to the color
lit all round themselves, like point lights. The cage stage sends three
spot lights of that kind every frame, and the scaffold stage has them
too.

**What is done.** A light's amount is one sum: its color, times its fall
with distance, times its cone. The routing says only where the amount
goes. So the cone section 64 read (full inside, nothing outside, by the
light's two numbers) now holds for every point light that has one, in the
four places that work a light out:

- `runtime/machine/view.c`, the processor's lighting;
- `runtime/host/shaders/scene.vert`, the graphics chip's;
- `runtime/host/shaders/rt_light.comp` and `rt_light.hlsl`, the two ray
  tracers' sum of the game's light (`light_cone`).

The light that goes to the alpha keeps its sums as they were, in the same
order.

**A second reading.** The Flycast emulator's code for this chip was read
(not copied) for how it takes the same numbers: it scales a point light's
color by the distance's fall and then by the angle's, before it looks at
the routing. That is the same order.

**Checked** (the Linux game in a headless desktop, every stage from its
snapshot, one picture in a fight, before and after):

- 22 of the 24 stages are the same to the pixel with the processor's
  lights, and the three looked at with the graphics chip's lights are too.
- The cage (stage 22): the picture changes all over. Before, the three
  spots lit everything evenly; now the ring's floor is lit and the crowd,
  the walls and the signs away from it are dim. The scaffold stage (3):
  the floor and the background are a little darker away from the lamps.
  Two more pictures later in a fight on each show the same.
- The graphics chip's lights against the processor's on the cage: one
  color step at most, as before the change; on the other four stages
  compared, the same to the pixel.
- With Vulkan ray tracing the two stages change in the same way.

**Not covered.** A comparison with the real board or with an emulator's
picture: that the cage is meant to look like this rests on the game's
own numbers and the second reading. DirectX 12 ray tracing (its shader
has the same change; it was not run). The other things section 64 lists:
the fog routings, the diffuse modes, the lights' second ambient.

## 90. The cave's traced water: less of a mirror (2026-11-26)

**Why.** The maintainer, playing with ray tracing on: the reflection in
the cave stage is too much, and the original has clear water there.

**The cause.** Section 81 lays a traced mirror over the game's own water
by the share an eye's angle gives a mirror on real water (2 % straight
down, most of it along the surface). In the cave (stage 20) the fight is
on a raft of poles over the water, and the camera looks along it: between
the far poles the mirror's share is a half and more, and what it mirrors
is the raft's own underside. The clear blue water with its stones, which
the game draws there, was filled with brown poles.

**What is done.** How much of that share the mirror takes is the stage's
matter (`gpu_render_water`'s second number, to `rt_light.comp` through
`tune2.y`): 25 % in the cave, all of it elsewhere, and the Shrine's pond,
which is a mirror in the game too, as it was. `--water-over N` sets it on
every stage, for the tests.

**Checked** (the Linux game in a headless desktop, stage 20 in a fight,
Vulkan ray tracing at Medium):

- At 100, 50, 25, 10 and 0 % and without ray tracing, side by side: at 50
  and 100 the far water is poles; at 25 it is blue water with a faint
  mirror; at 0 it is the game's water alone.
- The build's own picture of the cave is the 25 % one to the pixel.
- The earlier check of the water and the traced pictures (section 81's)
  with this build: 12 of its pictures are the same to the pixel, the
  harbor's and the Shrine's among them; those that changed are the
  cave's with ray tracing on (this section) and the cage's and the
  scaffold stage's (section 89).

**Known.** 25 % is by eye, from pictures of one moment of one fight, and
without a picture of the arcade game beside it.

**Not covered.** The cave from other camera angles and in its intro.
The Windows game.

## 91. A fence behind the fighters fades too: the game's own numbers (2026-11-26)

**The question.** The maintainer saw the foreground fence turn
transparent with the fighters in a corner, as it should, and the fence
behind them on the corner's far side turn transparent as well. It may
have been the cage. If the arcade game does the same, it is to stay.

**What the fade is.** Section 64's light: the game's light 14
(`extra_light_spot_set` in the decompilation), a spot that stands behind
the camera and looks along the view, whose amount is taken from the alpha
of a stage's fences and near objects while they are drawn. Nothing in it
knows where the fighters stand. It fades what is near the camera and near
the middle of the view.

**Its numbers on the cage** (read from the game's memory in the headless
game, with two fighters walked to the ring's edge and along it, at six
moments; they were the same at all six):

- The light stands 5.00 units behind the eye and 0.21 above it.
- Its amount is full up to 8.40 units from itself and gone at 8.80: on
  the view's axis, from 3.40 to 3.80 units in front of the eye.
- Its cone is full within 14.1 degrees of the axis and gone beyond 22.5
  (on Sarah's stage section 64 found 7.6 and 19.2).
- The camera stood 3.45 units from its target, the point between the
  fighters, each time: its closest.

So the zone that fades ends a third of a unit behind the middle of the
fighters. A fence they stand against is partly inside it, and in a corner
the fence at their side, which runs towards the camera, is inside it for
all of its near part. That is what was seen.

**Why this is taken for the arcade's own behavior.** The numbers are the
game's, sent to the graphics hardware as they are; the renderer here
works the amount out for each corner of a polygon, as that hardware's
transform and lighting chip does. The Flycast emulator's code for the
chip, read for section 89, takes the same record the same way: the fall
with distance, then the cone, then the amount off the alpha. No picture
of the real game in a corner was at hand to lay beside it.

**Checked.** Pictures of those six moments (the software picture): the
far fence is whole behind fighters who stand in the ring, and thins a
little in the middle of the view where they stand right against it.
Translucent polygons do not write depth in the GPU renderer and are
sorted from the back by triangle, so a faded fence in front cannot hide
one behind it.

**Nothing is changed.** A fade that only took what stands in front of
the nearer fighter would be a departure from the game, to be made only if
it is wanted.

## 92. A census of the game's light commands (2026-11-26)

**Why.** After the cone (section 89) the maintainer asked what else of
the lighting is still to be done. Section 64 lists what the renderer does
not do with a light: the diffuse and specular modes, the routings other
than the two it knows, the fog routings, a second ambient color, the
masks it merges. Whether any of that matters depends on what the game
sends, which nobody had counted.

**What is done.** `--light-census` (`runtime/machine/view.c`,
`light_cmd`) counts every light set header and every light the game
sends, by kind, and the game writes the count into its log as it ends
(`lights:` lines). The names of a header's bits are the Flycast
emulator's reading of the chip (read, not copied).

**The count** (every stage from its snapshot: the stage's intro and the
start of a fight):

| Sent | Times | Stages |
|---|---|---|
| Parallel light, to the base color | 492,135 | all 24 |
| Point light that falls with distance, to the base color | 67,674 | 3, 16, 17, 20, 21, 22 |
| Point light with a cone, to the base color | 29,592 | 3 |
| Point light with a fall and a cone, to the base color | 26,970 | 0, 7, 22 |
| Point light with a fall and a cone, its amount off the alpha (section 64's) | 19,098 | 0, 1, 2, 3, 4, 16, 19, 20, 22 |

Every light that goes to a color has diffuse mode 0 and specular mode 0
and routing 0. No light goes to the offset color or to the fog, none is
taken away from a color, and no header has an ambient for the offset
color or masks that differ. So of section 64's list nothing is used in
these runs, and with the cone every kind of light the game sends here is
one the renderer works out.

**What the count turned up instead.** Every light set header, on every
stage, has one bit set that the renderer does not look at: by Flycast's
reading, light over full brightness is carried into the offset color (it
calls the bit `useBaseOver`). Beside it stands how the renderer holds
the sum: it adds the ambient and the lights, holds the sum to 1, and then
multiplies the material's color by it. Flycast multiplies first and
holds the product to 1, with what is over going to the offset color when
that bit is set.

The two differ wherever the lights sum to more than 1, which the game
arranges on purpose: the rooftop's ambient is 0.89 to 0.98 and its sun
1.25 (section 89's trace). Held to 1 before the material, every face the
sun reaches at all comes out at the material's own color and every face
it does not at nine tenths of it: a stage with next to no shading. That
fits what players say of the picture: flat, the colors muted.

**This is a suspicion, not a finding.** Nothing was changed and no
picture was made. What would settle it is a picture of the game from an
emulator that does it Flycast's way, beside ours, and the change tried
behind a switch.

**Not covered.** The stages later in a fight, the other rounds, the
attract mode and the menus (other light sets may come there). The
specular light: where the game's routing sends it (to the base color, by
Flycast's names) against where the renderer puts it (the offset color)
was not looked into.

## 93. Light over full brightness: a trial (2026-11-26)

**Why.** Section 92's suspicion, tried: the renderer holds the lights'
sum to 1 and then multiplies the material's color by it, and it does not
read the bit every light set of the game has set, by which light over
full brightness goes into the offset color.

**What is done.** Behind `--light-over`, and only there:

- The material's color is multiplied by the ambient and the lights
  first, and the product is held to full.
- Where the light set's header has bit 9 and the polygon has an offset
  color, what the product had over full is added to the offset color,
  which the texture's stage adds after the texture.

In the processor's lighting (`runtime/machine/view.c`) and the graphics
chip's (`scene.vert`; the strip's record says whether it has an offset
color, the light set's fourth number which of the three ways it is).
The ray tracers' own sums are not changed, so the trial is to be looked
at with ray tracing off. Without the option nothing is different.

**What it does** (every real stage from its snapshot, one picture in a
fight, with and without, side by side):

- The stages get a little brighter where the sun or a lamp reaches: the
  harbor, the rooftop and the wall stage most (their pictures' mean
  brightness rises by 7 to 10 of 255), most others by 0 to 5.
- The fighters change most. Their cloth and skin take a sheen in the
  color of the light, and on stages with strong colored lights they are
  tinted by them: in the cave and in the snow a fighter's dark trousers
  come out blue and a red shirt pink; on the Shrine at sunset the
  trousers go dark blue.
- The graphics chip's lights give the processor's picture to the pixel
  on three of four stages compared, and within two color steps on the
  cage.

**The highlight: the trial's second part.** A picture the maintainer
supplied set the direction: a frame of a video of the game, Akira in his
white gi against Sarah on the ruins (a wide picture whose 3D view is wide
too, so most likely from an emulator and not from a cabinet). Beside ours
of the same match the gi is white there and middle gray in ours, and the
first part of the trial makes it only a little lighter.

The scene's numbers (a temporary dump of every material with its lights,
taken out again): the gi's base color is 0xCC (80 %), its offset color
0x7F (half), its gloss byte 0x60. The fighters' lights: ambient 0.40, a
white parallel light of full strength from above, a blue one (0.30, 0.50,
0.76) from below and behind, two small spots of 0.24. The stage's:
ambient 0.27, a white parallel light of 0.90 and a faint blue one, and no
offset colors. So the cloth is made to be lit over full (0.8 x 1.4) and
to take a wide highlight on top of that.

How the renderer makes the highlight, and the other reading of the chip's
words (the one section 92 took the header's bit from):

- Its width. The renderer: the library's own table (`object_put_init`):
  with c the cosine between the normal and the direction half way between
  light and eye, (2c^2 - 1)^2 to the power N, N the table's row for the
  material's byte. The other reading: the cosine between the eye's
  direction mirrored at the surface and the way to the light, to the
  power the gloss byte holds as a small float (3 bits of exponent e, 5 of
  fraction f: 2^(e-1) (1 + f/32)). That float is N itself: the library's
  table of bytes (`material_param_byte_table`) holds its rows' numbers in
  that form (0x20 is 1, 0x40 2, 0x50 3, 0x60 4, 0x68 5, 0x88 10). And
  2c^2 - 1 is about that mirrored cosine, so the renderer's highlight has
  twice the power: it is about 0.7 times as wide.
- Where it goes. The renderer: times the material's offset color, into
  the offset color, which is added after the texture; only where the
  polygon has an offset color and the light reaches the surface from the
  front. The other reading: times the material's offset color, into the
  BASE color (routing 0, which every color light of the game has), before
  the base color is held to full, what is over going into the offset
  color; with no condition on the polygon or on the light's side.

Behind `--light-spec N` (it turns `--light-over` on): the other reading,
with the power the byte says (N = 1) or twice it (N = 2, the library's
table). In the processor's lighting only: with the option a strip that
has a highlight is lit by the processor, whatever Lighting says.

What it does:

- Akira's gi comes out white with gray folds and a bluish edge away from
  the main light, as in the picture; with N = 2 it lies between. The
  whole picture beside the maintainer's is close; the stage is a little
  lighter and softer there, which a video's levels can do.
- All 15 real stages, one picture each, with and without (N = 1): every
  run ends well and nothing breaks. In daylight (rooftop, ruins, harbor,
  wall stage, temple) the fighters get brighter and rounder. Under strong
  colored lights (snow, cave, Shrine, scaffold) dark trousers come out
  blue and a red shirt pink, as in the first part: that is the light over
  full, not the highlight.

Measured against the maintainer's picture (ours with N = 1, both at one
scale, patches blurred alike so that the cloth's weave, which the video
has smoothed away, does not count):

- What no light touches is the same in both, so the video's levels are
  ours: the life bar's green 245 against 242, the sky's median and 90th
  level 151 and 240 against 144 and 239.
- The same floor tile between the fighters (a material with no highlight
  color): 168 as the game is, 189 with the trial, 191 in the picture.
- A patch of the gi over the hip that is cloth all over: mean color 151
  151 163 against 151 151 164; the levels a quarter, half and three
  quarters of its pixels lie under 133, 147, 170 against 130, 146, 170.
  With N = 2 the patch's mean is 138, as the game is 120.
- The picture's brightest twentieth of that patch is a little brighter
  (215 against 209). Eight of our pictures 0.2 s apart, as Akira sways in
  his stance, go from 204 to 215 there: the stance does more than that.
- Not the cause of anything: the internal resolution (1, 2 and 4 give the
  same levels) and bump maps (the scene has none).

**Against the real board.** The maintainer then supplied three frames of
a video whose title says it was recorded from the real hardware (4:3, with
the stair-stepped edges of 480 lines; the first picture, wide and of
another match, cannot be from it): Jacky against Goh in the snow, twice,
and Jacky against Brad in the cave. The same two matches in ours (the
same costumes), as the game is and with the trial:

- The life bar's green is the same in the video and in ours (249 against
  247 of 255), so its levels are ours.
- The cave. As the game is, Brad's trousers and shoes are black and his
  shirt plain red; Jacky's jacket and trousers are black leather. On the
  board the trousers are blue where the light reaches and dark where it
  does not, the shoes blue, the shirt red with a pink sheen, Jacky blue
  all over. With the trial they are that. The same pieces of Brad, as the
  game is, with the trial, on the board: the shirt 106 11 11, 106 14 43,
  111 19 48; the front shoe 28 34 34, 42 103 154, 33 71 119; the lit
  front thigh 40 46 44, 40 49 84, 68 91 121.
- The snow. As the game is, Jacky's leather is black and a band on Goh's
  sleeve a black block; on the board and with the trial both are blue.
- So the colored light on dark cloth, which the first part left open as
  perhaps too much, is what the board shows.
- These pieces were read off each picture by eye, at places that are not
  the same in both. Two differences they seemed to show (Brad's lit thigh
  and the open snow brighter on the board) went away when the frames
  were laid on ours place by place: see "The board's frames laid on
  ours" below.

**Against the emulator, running.** The emulator this reading comes from
was then run itself on the same two matches: the maintainer's own build,
in a headless desktop, on copies of its settings, with no network and no
sound; its script interface presses the buttons and sets the game's stage,
characters and costumes as the headless game's script does. At 960 lines,
beside ours of the same moment:

- The stage is the same in both to a few counts: the cave's rock 67 76 68
  against 68 76 68, the snow stage's wall 45 58 71 in both, the open snow
  84 106 148 against 80 101 140.
- The fighters, with the trial's first two parts, are the same where the
  material is plain cloth (Brad's shirt 106 14 43 against 107 14 42, his
  lit thigh 40 49 84 against 41 52 85) and too bright in ours on shiny
  parts: his shoe 42 103 154 against 34 70 103, the board's 33 71 119.
- The cause of that: the emulator gives the polygons of an
  environment-mapped material (the material's select bit 11) no offset
  color, and lets their texture's and their vertices' alpha count. The
  renderer added the highlight and the light over full there as anywhere.

**The trial's third part.** Behind `--light-env`: that rule, where a strip
is queued (the polygon word's offset bit cleared, the two alpha bits
set). With it Brad's shoe is 35 74 108, and of a dozen pieces of both
fighters and the stage ten lie within 8 of the emulator's, seven of them
within 5; the other two (the back of Jacky's jacket 11 darker, a sleeve
of his 23) are on parts that sway.

What else the emulator's source does differently, and makes no difference
in these scenes (by a dump of every material of the three matches):

- A vertex's two own color words. The emulator: the base colors outside
  and inside a shadow volume, used when the material's select bits 0 and 4
  are clear, and lit unless bit 9 says constant. The renderer: base and
  offset color, not lit. Only the cave's baked stage has them (half its
  vertices, constant); the second word is 0 there, or black on one
  material.
- Polygons with no texture, whose offset color the emulator adds into the
  base: there are none; nor lit polygons without the offset bit.
- A light's direction: 12 bits and brought to length 1 there, 8 bits here.
- The same in both: the matrices' turned axes, the fall with distance, a
  spot's cone (for the cones the game sends), shadows by the scale
  register, fog by the table.

**The board's frames laid on ours.** The HUD is drawn alike by the board
and by ours, and no light touches it. That gives two things.

Where the frames lie. From the two life bars' ends, and then from where
the two HUDs' brightness agrees best, ours' pixel (x, y) is the video's
(1.5926 x - 21.6, 1.583 y - 17.0), the same in all three frames: the
video is the same picture, without 14 columns at the left, 11 at the
right, 11 lines at the top and 74 at the bottom of ours' 1280 x 960.
Brought to ours' geometry by that, the board's frames lie on ours pole
for pole: the camera is the same.

Its colors. Anchors the video's blur cannot touch: lines of the life bars
and of their frame that have one color for hundreds of pixels and
neighbors of nearly the same color, and the time box's black and white
(0 and 255 in the emulator's digital picture): 74 of them, from 3 to 254.
A straight line per channel through them: ours = -0.3 + 1.001 x for red,
-5.2 + 1.024 x for green, -1.7 + 1.008 x for blue. The video's colors are
right to about 5 of 255 as they are, and the correction moves nothing
further than that. (An earlier reading here, that the capture lifts the
dark tones and tints the middle ones, took the video's blur at the HUD's
sharp edges for a tone curve: wrong.)

The same boxes, at the same place in every picture: as the game is, with
the trial's three parts, the emulator, the board's frame corrected:

    the cave
    bamboo floor, left     76  82  75    82  95  96    87 100 101    89  96 103
    bamboo floor, center  111 107  87   111 107  87   117 112  91   120 108  93
    rock                  100 112 109   107 116 109   107 116 110   110 113 112
    ice behind Jacky      150 171 175   158 175 175   158 176 176   158 166 172
    Brad's shirt          111  12  12   111  21  54   111  18  51   110  14  44
    Brad's front thigh     44  49  47    44  58  87    45  60  89    47  49  70
    Brad's back leg        26  25  23    26  26  26    25  26  27    34  29  31
    Brad's front shoe      23  29  29    25  57  90    25  54  86    29  64 114
    the snow
    open snow             101 128 179   101 128 179   100 127 177   100 121 181
    the fighters' shadow   59  75 105    59  75 105    58  74 104    60  70 107
    tower wall             41  53  60    41  53  60    42  53  61    47  52  64
    Goh's trousers         42  79 137    42  87 157    49 101 171    48  93 169

- The stage is the board's to within 10 with the trial. As the game is it
  is so too, but for what is lit over full: the cave's floor at the left
  is 28 short of blue, its rock and ice 8 to 10 dark.
- The fighters, as far as two frames a moment apart can say (they sway):
  with the trial Brad's shirt is 10 from the board's, his shoe 24, Goh's
  trousers 12; as the game is they are 32, 85 and 32 away. Brad's thigh
  is 17 away with the trial and 23 as the game is.
- The shadow is as dark in all of them: 0.58 of the lit snow.
- The two differences read off by eye before are gone: at the same place
  Brad's thigh is 47 49 70 on the board and 44 58 87 with the trial; the
  open snow 100 121 181 against 101 128 179.
- What is left: the board's snow has 7 less green than ours and the
  emulator's. The video's light gray is as much short of green against
  its red and blue (the HUD frame's 221 221 221 comes as 227 220 226), so
  this is most likely the capture's, at the edge of what the anchors can
  tell.
- The first picture (the other source) by the same method: its low reds
  and blues are lifted even on flat lines (42 comes as 50 to 61), and its
  lines are ours = -15 + 1.09 x for red and blue, -6 + 1.03 x for green.
  Corrected, the gi's patch over the hip is 149 150 164 against the
  trial's 151 151 163.

**How far off, in numbers.** Against the board's corrected frames, over
the stage: every pixel below the HUD band and above the bottom strip,
with the time box, the round's letters and a generous rectangle around
each fighter left out (a fifth of the picture stays), both pictures
averaged over 33 x 33 pixels so that the video's softness does not
count. The mean difference per channel of 255, the signed mean (picture
minus board), and the median CIEDE2000 distance:

    the cave
    as the game is    8.5 2.9 4.3    -8.4 -0.9 -4.0    5.1
    with the trial    6.6 2.9 4.3    -6.3 +0.1 -4.0    4.7
    the emulator      4.0 3.6 2.7    -3.2 +2.6 -1.7    4.6
    the snow
    as the game is    3.1 5.6 9.4    -1.0 +4.6 -8.3    3.8
    with the trial    3.1 5.6 4.5    -1.0 +4.7 -2.9    3.1
    the emulator      2.2 5.2 4.7    -0.5 +5.1 -3.3    2.9

- With the trial the stage is 1.7 to 1.8 % of the full range from the
  board; the emulator 1.3 to 1.6 %; as the game is 2.1 to 2.4 %. The
  trial against the emulator, measured the same way: 1.0 to 1.1 %, a
  median distance of 0.6 to 0.8.
- Most of what is left is one shift that ours and the emulator share:
  against its red and blue the board has 5 to 7 less green than either.
  The HUD keeps the same after the correction (its light gray,
  221 221 221, stays 227 220 226), so it comes with the board's output
  or the capture, not with the lighting. The correction's lines miss
  their own anchors by 5.0, 3.2 and 6.5 (root mean square; red, green,
  blue), and the anchors are few colors (black, the bars' greens, one
  light gray, white): no red or blue between 50 and 130 is pinned. A
  difference of 5 is at the edge of what these frames can tell.
- The fighters' cloth, by the boxes above (Brad's four and Goh's
  trousers), the largest channel's difference averaged: 14 with the
  trial, 14 for the emulator, 36 as the game is.
- One difference is not the capture's. The cave's bamboo floor is darker
  in ours than in the emulator, away from the fighters and their
  shadows, with the trial or without. Ours, the emulator, the board:
  far left 98 101 95, 105 106 100, 110 104 104; far right 83 81 70,
  90 87 75, 92 83 75; behind the fighters 97 92 77, 104 97 80,
  102 91 79. The rock walls behind are alike in ours and the emulator
  (26 30 22, 27 30 22), and so is the snow's stage. Looked into since:
  section 94.
- (An earlier whole-picture measure left only the right half of the time
  box out; this one leaves all of it.)

**Not decided** (then; decided since: section 98, simply so). Whether this
becomes the game's lighting, and how (a setting, or simply so). For it now: the real board's frames on two stages
with strong colored lights, the emulator's own pictures of the same
matches, and that the materials' numbers make no sense without it (a
white gi has an 80 % base under 140 % of light). Which width the
highlight has on the chip is not settled by the board's frames; the
emulator's is the byte's own power (`--light-spec 1`).

**Not covered.** The graphics chip's lights (`scene.vert`) and the ray
tracers with the second part (the third works with both lightings: it
changes the polygon's words); ray tracing with the first. The Windows
game (it builds).

## 94. Vertex colors across a triangle: on the screen here, with the perspective on the board (2026-11-26)

**Found here; changed in section 95.**

**What was seen.** Section 93's last measure left one difference that was
not the capture's: the cave's bamboo floor is darker in ours than in the
emulator and on the board, most in a band across the middle of the raft,
with the trial or without.

**What it is not** (each tried):

- The lights. The raft's poles have colors of their own in every vertex
  (baked; they take no light). With the point lights' fall with distance
  off, the point lights off, both sides of a face lit, or the parallel
  light without the surface's angle, not a number on the floor changes.
- The texture's smaller levels. The floor's texture (128 x 128, with
  levels) has the same mean color in every level (160.5 142.3 121.7 at
  128, 160.7 142.4 121.9 at 64, 160.6 141.9 121.2 at 32), and the
  software renderer, which takes the levels, draws the floor as dark as
  the GPU renderer, which takes the largest only (at internal resolution
  1 and 2 alike).
- Fog. The difference is a gain (dark, middle and bright poles go by the
  same factor: 1.01 near the camera, 1.10 further, 1.15 in the band),
  not a step toward one color.

**What it is.** Both renderers carry a vertex's colors across a triangle
on the screen: the software rasterizer steps them a pixel at a time
(`raster()` in `runtime/machine/view.c`), and the GPU's shaders declare
them `noperspective` to match it (`scene.vert`, `scene.frag`). The
emulator carries them with the perspective, as it does the texture's
coordinates: its shaders multiply the colors by 1/w at the vertex and
divide in the pixel (read, not copied). On a long triangle that runs away
from the camera with different colors at its ends the two ways agree at
the ends and differ in between: the raft's poles.

**The test.** The two `noperspective` words taken off the colors (the
depth keeps its own), the shaders made again, the cave and the snow
drawn. The poles' mean color in a box: as the game is, the test, the
emulator, the board:

    far left, lines 650-700     94  88  74   106  98  83   108 100  85   108  96  89
    far left, lines 700-750    122 114 101   135 124 110   135 125 111   134 118 110
    far right, lines 700-760   116 107  92   126 115  98   126 115  99   120 105  95
    behind the fighters         98  92  77   103  96  80   104  97  80   102  91  79
    near, lines 760-850        111 103  89   114 105  91   113 104  90   123 107  99

- Over the cave's stage, by section 93's measure, the trial with the
  test is 0.6 % of the full range from the emulator (1.1 % before; 97 %
  of it within a distance of 2) and 1.5 % from the board (1.8 % before;
  the emulator 1.3 %).
- In the five floor boxes of section 93, in red and in blue (the
  capture's green apart), the board is nearer the test than the game as
  it is in all ten.
- The snow does not change.

**What else it would change** (every real stage from its snapshot, one
picture in a fight, the test against the build before): the cave (20:
17 % of the pixels by more than 2 of 255, an area by up to +22 +17 +14),
22 (19 %; -3 -11 -8), 16 (9 %; -19 -15 -12), 18 (5 %), 3 (4 %), 19 (3 %);
the other nine in 0.3 % of their pixels or less.

**What a change needs** (as it stood when this was written, before
section 95; none of it done here; the test is taken out again
and the shaders are what they were, byte for byte):

- The GPU renderer: the two words, and a guard for rounding. The test
  darkens every stage by a fraction of a step (a mean of 0.1 to 0.4 of
  255): a color that is the same in all three vertices no longer comes
  out exact, and the shader cuts it to the whole number below.
- The software rasterizer the same way (its colors exact every eighth
  pixel, as its texture coordinates are), or the two renderers part.
- The reference pictures of the stages above made again.

**Not covered.** The test did all four parts of both colors (base and
offset, the alpha with them), as the emulator does; whether the board
does it for each was not told apart. One camera and one frame a stage. A
statement of the chip's maker was looked for and not found: the
emulator's reading and the board's frame are what this rests on. The ray
tracers' own pictures were not looked at.

**Is ours darker everywhere?** Asked of the floor's table, where the
emulator and the board are brighter in every row. By part of the picture
and by tone level (the stage as in section 93, averaged over 9 x 9; ours
with the trial minus the emulator, then the emulator minus the board):

    the cave, walls and scaffold    -0.9 -0.8 -0.8     -3.7 +1.7 -2.1
    the cave, floor                 -7.3 -5.6 -5.1     -2.9 +3.6 -1.7
    the cave, floor, with the test  -1.7 -1.3 -1.4
    the snow, buildings and sky     -1.3 -1.2 -0.4     -1.6 +3.8 -3.0
    the snow, ground                +0.1 +0.3 +0.9     +0.7 +7.0 -2.6

- Against the emulator ours is about 1 of 255 darker nearly everywhere,
  at every tone level alike, the floor apart. The HUD has it too (a life
  bar 76 248 73 against 77 248 73), so it is not the lights. The pixel's
  sum is cut to the whole number below at three places (the color, the
  filtered texel, their product: `scene.frag`, after `raster()`), and
  the emulator keeps the fractions to the end. Tried (the fractions kept
  through the GPU shader's color sums and rounded once, by the picture's
  own 8 bits; taken out again, the shaders byte for byte what they were):
  ours minus the emulator goes to 0.0 +0.2 0.0 on the cave's walls and
  -0.3 -0.1 +0.7 on the snow's buildings, the HUD's boxes come out the
  emulator's to the number (77 248 73; 205 98 90), and a whole picture
  gets 0.8 to 0.9 brighter. So the 1 is the cutting. It is not known to
  be the chip's rule: it is how the software rasterizer was written (its
  blend rounds to the nearest, its texture times color cuts), and the
  GPU's shader follows it. Which the chip does a step of 1 cannot be read
  off a video good to 5.
- Against the board the emulator and ours differ alike, and by tone: in
  the darkest fifth of the range the board's frame is brighter (the cave:
  6.7 1.5 3.7; the snow: 4.0 -0.4 4.4), in the bright tones it has 7 less
  green and red and blue within 3. The HUD's known colors go the same way
  in the corrected frames (black reads 3 0 5, light gray 227 220 226), so
  most of this comes with the board's output or the capture; the HUD has
  no red or blue between 50 and 130 to say how much.

## 95. Vertex colors with the perspective; the pixel's sum keeps its fractions (2026-11-27)

**Why.** Section 94 found two things the renderers did that the board and
the emulator do not. A vertex's colors were carried across a triangle on
the screen, not with the perspective (the cave's floor too dark in a
band). And the pixel's color sum was cut to the whole number below at
every step, which made every picture about 1 of 255 darker than a sum
that keeps its fractions. The maintainer's word on both: fix them, in
every renderer.

**What is done.**

*The software rasterizer* (`raster()` in `runtime/machine/view.c`):

- The colors with the perspective, the way the texture's coordinates
  already went: color / w runs straight on the screen; every eighth
  pixel a stretch's two ends are worked out exactly (each its color / w
  over its own 1 / w) and the pixels between go straight. The base color
  and the offset color both. A triangle of one depth (the HUD, a
  picture) or of one color keeps the plain way, which is the same there.
- The pixel's sum carries 8 more bits below the point (0 to 65280 for 0
  to 255) from the vertex color to the write, and is rounded once, to the
  nearest, as the pixel is written: the vertex color; the four texels'
  mix (across in 16 bits a channel, down in 32, nothing cut between);
  texture times color (the product over 65280, to the nearest); the
  mix by the texture's alpha; the offset color's sum; the fog's step;
  the shadow's scale on the translucent list; the three blends. The
  alpha test compares with its reference times 256.
- The opaque list's shadows (the color times the scale, after the list)
  round to the nearest. The background plane (`target_clear`) keeps its
  fractions through its fog and rounds once.

*The GPU renderer:*

- `scene.vert`, `scene.frag`: the two colors are no longer
  `noperspective` (the depth still is). The pixel's sum has no `floor`
  left in it: the vertex colors are taken to the nearest 256th, as
  `raster()` carries them; the texel's mix, the product, the mix by
  alpha, the fog's step keep what they have; the picture's own 8 bits
  round once.
- A vertex's colors go to the GPU as bytes: to the nearest now
  (`gpu_rgba`), and the colors the GPU lights itself (section 67) round
  the same way, so the two lightings stay the same picture.
- `background.frag` as `target_clear`.

*The ray tracers:* nothing to change. What a ray hits is shaded on the
triangle in space (with the perspective by nature) and in fractions
already (`rt_light.comp`, `rt_light.hlsl`); what the eye sees first is
the rasterized picture, so it takes the change from the two shaders
above.

**Kept as they were.** The weights, which are not colors: the texels'
mix in 256ths of a texel, the fog table's step with its `f + f / 128`,
the whole-number amount of the fog by vertex. The blends other than the
three common ones take their weights from the colors rounded.

**Checked** (the Linux dev build in headless desktops; the cave and snow
matches of section 93 at 7450, and every real stage from its snapshot at
7600):

- Against the emulator, the trial's three parts on (section 93's measure,
  the stage averaged over 33 x 33): the cave 1.1 % of the full range
  before, 0.3 % now (ours minus the emulator -0.2 +0.0 -0.1; its walls
  -0.1 +0.1 -0.0, its floor -0.3 +0.1 +0.0, where it was -7.3 -5.6
  -5.1). The HUD's boxes are the emulator's to the number (a life bar
  77 248 73, a name plate 205 98 90). The snow: 1.0 % before, 1.2 % now
  (see "Left").
- Against the board's corrected frames: the cave 1.8 % before, 1.4 % now
  (the emulator 1.3 %); the snow 1.7 % and 1.7 % (the emulator 1.6 %).
- The lights on the GPU against the processor's: the same picture, pixel
  for pixel (the cave, the snow).
- The software renderer against the GPU's at internal resolution 1: the
  cave before: ours 0.2 darker, 62.7 % of the pixels not the same, 4.5 %
  by more than 2. Now, on stages 0, 3, 16, 20, 22: within 0.06 on
  average, 34 to 46 % not the same (by one step), 2.7 to 4.1 % by more
  than 2 (edges). The two were never the same to the pixel; they are
  nearer now.
- Every stage against the build before: brighter by 0.2 to 1.4 on
  average (22's green and blue stay level: the perspective takes there
  what the rounding gives); more than that in places where the
  perspective counts: the cave (20: a
  fifth of its pixels by more than 2, an area by +23 +17 +15), 22 (17 %;
  -2 -10 -7), 16 (8 %; -18 -14 -10), 3 (8 %; -14 -4 -4), 19 (9 %), 4
  (11 %), 0 (9 %).
- Ray tracing (Vulkan, Medium and High) on stages 20, 22 and 3: runs; the
  pictures of 20 and 22 looked at beside the plain ones. There are no
  pictures of the build before to lay them on (section 80's are older
  than section 89).
- The Windows build compiles. It was not run.

**What it costs.** The software renderer: 6.1 ms a frame before, 7.1 ms
now (the cave match's 599 frames as fast as they go, four runs each in
turn, 6.01 to 6.12 against 7.05 to 7.23; the renderer before built beside
the tree from the file at HEAD, its picture the old build's to the
pixel). The time is the wider sum, not the perspective: with the
perspective switched off it is the same 7.0. A shortcut tried and taken
out again: the plain way for triangles where the perspective could not
move a color by a quarter of a step; it brought nothing. The GPU
renderer's time was not measured (two words less to interpolate flat, a
few `floor` less).

**Left.**

- Every reference picture of an earlier release is off by about 1 now,
  and stages 20, 22, 16, 3, 19, 4, 0 by more: the next release's picture
  checks need all of them new.
- With the cutting gone, what the lights reach is about 0.7 brighter
  than the emulator's on the snow's ground (ours minus the emulator, the
  bright ground, as the game is: -0.5 -0.4 -0.7 before, +0.7 +0.8 +0.7
  now), where baked colors (the cave) are level. A guess, not tried: a
  light's color is its byte over 127 here and twice its byte over 255 in
  the emulator (0.4 % apart), and a light's direction is 8 bits not
  brought to length 1 here (up to 0.8 % long). Which the board does is
  not known. Looked into: section 96.
- The software renderer's speed, if it matters: the four texels' mix and
  the product are where SSE2 would pay.

**Not covered.** Whether the board rounds or cuts (a step of 1 cannot be
read off a video good to 5: section 94); rounding to the nearest is the
choice that leans neither way. One frame a stage, 4:3, internal
resolution 2 (and 1 for the software pair). The ray tracers' pictures
against any before. DirectX 12. Windows itself.

## 96. A light's direction: 12 bits in its command, 8 read here (2026-11-27)

**Status.** Found here and changed the same day ("Changed", below).

**Why.** Section 95 left one thing open: with the cutting gone, what the
lights reach read brighter than the emulator's picture, where baked
colors were level. The maintainer asked for it to be looked into.

**How the game's library sends a light** (read in the decomp:
`elan_light_cmd_build`, `elan_light_pack_directional`,
`elan_light_pack_point`, `elan_light_pack_spot`):

- The color: each float times 127 into a byte. So 127 is a light of 1,
  and a byte reaches 2.
- The direction: each float times 65535, and from that, for the newer
  ELAN (which the runtime says it is: `hw_gfx.c`, revision 2), 12 bits a
  component: the 8 high bits in the direction's word, the 4 below them
  in the command's first word (bits 16 to 19 for x, 4 to 7 for y, 0 to 3
  for z).
- `elan_light_pack_spot` then measures the direction as it packed it,
  the 12 bits over 2048, and multiplies the cone's two cosines by that
  length before it makes the cone's two numbers from them. That only has
  a purpose if the chip takes the direction as sent and does not bring
  it to length 1; and it says what the chip's 1 is: 2048.

**What the renderer reads** (`light_cmd`): the direction's byte over
127, and the 4 bits in the first word not at all. The byte is the 12
bits cut downward, so a component below zero comes out larger than it
was, and 127 is not the scale: a direction with all three components
below zero (a sun that shines down and away) is read 1 to 2 % too
long. The snow match's sun, from its commands: 1.013 to 1.019 by the
byte, 1.0003 to 1.0005 by the 12 bits. A parallel light's amount goes
with that length; so does the cosine a cone is cut by, while the cone's
two numbers were made for the 12 bits' length.

**What the emulator reads:** the 12 bits, brought to length 1; and the
color as twice the byte over 255, which is 0.4 % under the byte over
127.

**Tried** (a switch in `light_cmd` for the test, taken out again; the
snow and cave matches of section 93 at 7450, the trial's three parts
on; ours minus the emulator):

| The lights read | Snow's bright ground (three boxes) | Snow, whole stage | Cave, whole stage |
|---|---|---|---|
| As now | +1.8 +2.1 +3.0 | 1.2 % (+0.8 +0.9 +1.8) | 0.3 % |
| The color as the emulator | +1.3 +1.7 +2.5 | 1.1 % (+0.5 +0.6 +1.2) | 0.3 % |
| The direction by the 12 bits | +0.2 +0.2 +0.3 | 0.8 % (-0.2 -0.3 -0.2) | 0.3 % |
| Both | -0.1 -0.2 -0.3 | 0.8 % (-0.5 -0.6 -0.7) | 0.3 % |
| Both, and length 1 | as both, to a step's rounding | the same | 0.3 % |

So the direction's length is more than four fifths of it and the color's
scale the rest. Section 95's "about 0.7" was the stage's average; on the
bright ground itself it is about 2. Two of section 93's boxes are no
measure of the light and are left out here: the fighters' shadows lie in
other places in the two pictures (the poses are not the same to the
frame).

**What the 12 bits change** (the test build; every real stage from its snapshot at
7600, the game as it is): every stage a little darker (0.03 to 0.9 of
255 on average), most on stage 2 (17 % of its pixels by more than 2) and
stage 4 (16 %). Larger in two places, neither taken apart further: a
roof's sheen on stage 4 (an area by -14 -10 -11; a highlight would go
with the direction itself, which 4 more bits move), and on stage 22 a
sign that loses most of a spot light (-44 -30 -23), which is what a
cone's edge moved looks like: the edge is where the library put it only
when the direction has the length the library measured, and by the byte
over 127 the cosine a cone is cut by is 1 % and more too large, against
a cone whose soft edge is a few hundredths of a cosine wide. The lights
on the GPU and the processor's stay the same picture, pixel for pixel
(the snow match).

**Changed** (the maintainer's word: fix it). `light_cmd` reads each
component as its byte times 16 plus its 4 bits, over 2048, not brought
to length 1. The GPU's lights and the ray tracers take a light from the
same record, so every renderer follows. The color stays the byte over
127, the library's own scale.

Checked (the Linux dev build in a headless desktop; the snow and cave
matches at 7450):

- As built against the test build's pictures with the 12 bits: the same,
  pixel for pixel (both matches, as the game is and with the trial). So
  the table's third row is the game now.
- The lights on the GPU against the processor's: the cave the same; the
  snow 5 pixels of 1,228,800 by one step (none before).
- The software renderer against the GPU's at internal resolution 1: as
  near as in section 95 (the snow within 0.08 on average, 3.6 % of the
  pixels by more than 2; the cave 0.02, 3.4 %).
- The Windows build compiles. It was not run, and ray tracing was not
  run again (it reads the same record).
- Section 95's word on the reference pictures holds the more: all of
  them new for the next release.

**Not covered.** The board's frames cannot tell these readings apart
(each is 1.7 % from the corrected frame, the emulator 1.6 %; a
difference of 2 is under what the video is good for). Whether the chip
divides a color's byte by 127, by 127.5 or by 128 is not known. Whether
it brings a parallel light's direction to length 1 is not known either,
and with 12 bits it is 0.05 % at most. The older ELAN's packing (8 bits,
measured over 127 by the library) never runs here.

## 97. The menu from a gamepad: View + Menu held, not the Guide button (2026-11-27)

**Why.** The maintainer: the Guide button (the Xbox, PS or Home button)
as a rule opens Steam's overlay, so it should not be the game's menu as
well; a combination instead: View + Menu, which is Share + Options on a
PlayStation pad and - and + on a Switch one.

**What was there.** Both. Guide opened and closed the menu at once, and
View + Menu held for a second opened it.

**What is done** (`runtime/host/main.c`):

- The Guide button is not the menu's any more, in the game or in the
  menu.
- The hold is half a second (`CHORD_MS`), where it was a second: it is
  the only way in from a gamepad now. It stays a hold and is not a press
  of the two together, because View is the coin and Menu the start
  button, and the two tapped one after the other overlap.
- A hold that begins in the menu opens nothing. Menu goes back a step
  there, and from the menu's first page that closes it; before, the same
  hold opened the menu again when its time was up.

As before: from the turn both are down until they are let go the two
buttons are kept from the game; the one pressed first is the game's
until the other comes down (a coin, or start). Insert and Esc are as
they were.

**Checked** (the Linux dev build in a headless desktop, at the game's
own pace, SDL's virtual gamepad, the snow match):

- Guide for a third of a second: no menu.
- View + Menu held for a second: the menu opens 30 vertical blanks after
  both are down (6908 to 6938: half a second), and Esc closes it.
- View + Menu for a fifth of a second: no menu.
- In the menu, View + Menu held for a second and a half: the menu
  closes with the press and is not opened again.
- The second run with a DualSense's ids: the same, the menu's foot
  showing that pad's buttons.

**Not covered.** Steam itself (that the overlay opens and the menu no
longer does). A real gamepad. Windows (the build compiles; not run). The
launcher has nothing that reads Guide.

## 98. The trial's lighting is the game's lighting (2026-11-27)

**Why.** The maintainer's word on section 93's trial: no options; its
three parts are right and what the renderer did before is not. The case
for it is in section 93: the board's own frames on two stages, the
emulator run on the same matches, and the materials' own numbers (a
white cloth of 80 % made to stand under 140 % of light).

**What is done.** The three options (`--light-over`, `--light-spec N`,
`--light-env`) are gone, and what they switched is what the renderer
does:

- The material's color times the ambient and the lights, plus its offset
  color times the highlight, and that product held to full; what is over
  goes into the polygon's offset color where the light set's header asks
  for it and the polygon has one. Before: the lights' sum was held to 1
  and the material's color multiplied by that.
- The highlight: the eye's direction mirrored at the surface, against
  the way to the light, to the power the gloss byte holds, whichever
  side the light is on; into the base color. Before: the library's own
  table (`object_put_init`), which is twice that power, into the offset
  color, and only for a light before the surface. The power is the
  byte's own (the trial's N = 1): the emulator's reading, which the
  first picture's cloth agreed with to a count (151 151 163 against
  151 151 164); the board's frames do not settle the width.
- An environment-mapped material's polygons have no offset color, and
  their texture's and their vertices' alpha count.

In every renderer:

- *The processor's lights* (`strip_job` in `runtime/machine/view.c`):
  the trial's code without its switches; the old highlight and the old
  hold are taken out. The software renderer draws from the same
  vertices.
- *The lights on the GPU* (`scene.vert`, the default since section 79):
  the same sum. The trial had left every strip with a highlight to the
  processor; now the shader works the highlight out too, from the
  strip's record (its eighth number is the gloss's power).
- *The ray tracers* (`rt_light.comp`, `rt_light.hlsl`): what a ray meets
  (the light that comes back from around, what glass and water mirror)
  is its color times the lights with the product held to full, where the
  sum was held. The light's ratio on the picture itself is as it was:
  the traced sum over the game's, which is the right factor for a
  product that is not held, and nearer the truth than before for one
  that is.

**Found on the way: a cut triangle lit by the GPU.** With the highlight
on the GPU, the wooden ring's floor (stage 17) came out up to 20 of 255
brighter with the GPU's lights than with the processor's, over a quarter
of the picture, and the cage's floor up to 4. The cause is older than
this section (section 79 noted 96,271 pixels of the cage a step apart):
a triangle that the near plane or the guard band cuts gets new corners,
and for a strip the GPU lights those carried a normal and a place
between the two vertices', so the GPU lit the cut point itself. The
board lights a model's vertices and cuts afterwards: a cut point's color
lies between its two vertices' colors. Near a highlight or a spot's cone
that is another color. Now a strip with a triangle that needs cutting
is lit by the processor (`tri_view` says so, `strip_job` does the strip
again): the floor under the camera and little else.

**Checked** (the Linux dev build in headless desktops):

- The snow and cave matches of section 93 at 7450, as the game is now,
  against the build before with the trial's three options: the same,
  pixel for pixel.
- The lights on the GPU against the processor's, every real stage at
  7600 and the two matches: the same on 13 stages and the cave match;
  22 pixels of 1,228,800 a step apart on stage 16, 1 on stage 22, 5 in
  the snow match. Before the cut triangles' fix: 354,152 pixels on stage
  17 (up to 20) and 342,625 on stage 22 (up to 4).
- The software renderer against the GPU's at internal resolution 1: as
  near as in section 95 (the snow within 0.08 on average, 3.8 % of the
  pixels by more than 2; the cave 0.02, 3.4 %).
- Ray tracing through Vulkan, Medium and High, on stages 20, 22 and 3:
  every run ends well.
- The software renderer's time on the cave match, four runs each in
  turn: 6.74 to 6.93 ms a frame before, 6.75 to 6.99 now.
- The Windows build compiles and both tracers' shaders are made again.

**Not covered.** The DirectX 12 tracer and the Windows game were not run
here (the release's check runs them under Proton). The ray traced
pictures were not laid on any before: the pictures of earlier sections
have the old lighting. The fighters' cloth on the other thirteen stages
against the board or the emulator: section 93 has two matches.

## 99. An audit of the renderer: the library's source, a census, the emulator on every stage (2026-11-27)

**Why.** Two faults of one week (a light's direction read short, colors
not carried with the perspective) had one thing in common: nobody had
laid that part of the renderer beside anything. The maintainer asked for
the rest to be laid beside the game's own library and beside the Flycast
emulator on every stage, the DirectX tracer included.

**How.**

- *Three readings of the sources*, each of one area: every ELAN command
  and field (what the library's packers write, what `view.c` reads, what
  the emulator reads); the pixel's path on the PowerVR chips (`raster()`,
  the GPU renderer's shaders, the emulator's); and environment maps, bump
  maps, the model compiler and the shadows. Between them some thirty
  places where `view.c` and the emulator or the library do not do the
  same.
- *A census of what the game sends* (`--state-census`, `view.c`,
  `state_census`): for every strip of a model and every polygon sent
  already transformed, by list, each value of each field of its control
  words, the strip's format and its material's flags; and the registers a
  frame is drawn by. Every real stage, from its intro into the fight. It
  sorts the thirty places into what this game does and what it never
  does.
- *The emulator on every stage, at the same game moment as ours.* The
  same pair of fighters and costumes forced on all 15 stages in both; no
  button pressed once the match is forced; pictures 111, 171 and 231
  vertical blanks after the fight begins (the game's main mode 3, sub
  mode 10: found in ours by the headless game's script, in the emulator
  by its own script). The emulator at 960 lines in a window of 1280 x
  960: its own pixels, not scaled. Compared outside the HUD and a
  rectangle around each fighter, both averaged over 9 x 9. (A first round
  without the same moment was no use on three stages: the camera had
  moved.)
- *The DirectX 12 tracer against Vulkan's*, the release's Windows game
  under Proton, every stage at Medium.

**Two faults found, and mended here** (not in release 0.5.26):

1. *A mip-mapped texture that is not compressed was read 2 texels
   early.* Its levels start at texel 3 (then 4, 8, 0x18, ...);
   `tex_decode` took the compressed kind's numbers (1, 2, 6, ...), which
   count index bytes. Every level was the right picture with its texels
   shuffled inside each block of four. The census has such textures on
   stages 1, 2, 11, 20 and 22; on the rooftop (stage 1) the office
   buildings' windows were streaks. With the 2 texels: 75,444 pixels of
   stage 1 change, 28,211 of stage 2, 20 of stage 11, and the windows
   are the emulator's.
2. *A vertex with colors of its own got no coordinates from its normal.*
   A material can ask for its texture to be laid on by the normals
   (environment mapping) on a strip whose vertices carry their own
   colors (format 0x42, the material's flags 0x1e00: stages 18, 20, 22).
   `strip_job` made those coordinates only for a vertex the lights
   reach, so every vertex of such a strip had the texture's corner. The
   cage's lamp is one: a glow in its own colors, blended by adding, with
   the glow's texture laid on by the normals. It was dark; it shines now
   as in the emulator (the same box of the picture at three moments:
   101 44 25 before at all three; 197, 169, 181 red now; the emulator
   209, 162, 189: it pulses).

Checked with both in: the lights on the GPU against the processor's on
stages 1, 2, 18, 20 (the same to the pixel) and 22 (one pixel a step
apart); the software renderer against the GPU's as near as before; of
the 15 stages' pictures of section 98's check, stages 1, 2, 11 and 22
change and the other eleven are the same to the pixel. The Windows build
compiles.

**Ours against the emulator, the same moments, both faults mended** (the
mean difference of the stage as a share of the full range, the best of
the three moments):

| Stage | Apart | | Stage | Apart | | Stage | Apart |
|---|---|---|---|---|---|---|---|
| 18 | 0.0 % | | 16 | 0.3 % | | 20 | 0.7 % |
| 11 | 0.2 % | | 1 | 0.4 % | | 23 | 0.8 % |
| 21 | 0.2 % | | 3 | 0.4 % | | 2 | 0.9 % |
| 4 | 0.3 % | | 5 | 0.5 % | | 19 | 0.9 % |
| | | | 0 | 0.6 % | | 22 | 1.1 % |
| | | | | | | 17 | 1.2 % |

What is left on the two last: stage 22's crowd and lamp move (they are
not at one phase in two runs); stage 17's floor is the next item.

**Found, and not changed: which of two ways is the board's is not
known.**

- *Cut first or light first.* The emulator cuts a strip at the near
  plane on the processor and lights the cut's new corners themselves;
  `strip_job` lights a model's vertices and lets a cut point's color lie
  between its two (section 98 brought the GPU's lights to that too). On
  a floor under the camera that has a highlight the two differ: stage
  17's wooden floor is 8 to 15 of 255 darker here than in the emulator,
  the snow's ground (stage 2) 7 to 17 at the picture's foot. A frame of
  a real board on either stage, at the foot of the picture, would
  settle it.
- *The depth test of the translucent list.* Every translucent polygon of
  this game is sent with "greater" (or "never", below), and so it is
  tested. The emulator tests "greater or equal" whatever is sent, in a
  list the chip sorts (this game's lists are: the feed register is
  0x00800408). A difference only where a translucent polygon lies
  exactly on what is behind it; none was seen in the 15 pictures.
- *The order of translucent triangles:* by the mean of their three
  depths here, by the nearest in the emulator.
- *The offset color's sum is held to full before the fog* here; the
  emulator holds nothing until the end. It matters more since section 98
  (light over full goes into the offset color), and only under fog.
- *Which mip level.* Here the level follows the polygon's D number as a
  factor (D / 4: stage 22 sends 10 for most of its polygons, which is
  1.3 levels softer); the emulator takes D above 4 for 4. The usual
  description of that field is a factor in quarters.
- *A shadow volume that the near plane cuts is left open* (the library's
  own software sender closes the hole; the emulator presses the volume
  flat onto the plane). It can only show with the camera low and close
  to a fighter; not looked for.
- *The far plane* in the matrix command is not read (the emulator drops
  a strip wholly beyond it).
- *The background plane's depth* (the emulator draws it at its depth;
  here the depth starts empty).
- Stage 11 alone sends trilinear filter modes, the second buffer's bits
  and a one-plus-one blend on a few strips a frame. Neither renderer
  does anything with the first two; the pictures agree to 0.2 %.
- From before: a light's color is its byte over 127 (the library's own
  scale; the emulator's is 0.4 % under); the highlight's width.

**What the census shows this game never sends** (so that a difference
there cannot show): bump maps; palette and YUV textures; fog by vertex
and the fog mode that replaces the color; polygons with two sets of
parameters; a depth test other than "greater"; depth writes switched
off; anything in the punch-through list or with "never" but the shadow
volumes' own models, which the library queues into every list; a user
clip. The color clamp's bit is set on a few strips a frame, with the two
clamp registers at 0 and 0xffffffff, which clamp nothing.

**Not faults.** Stage 18's fighters are translucent blue shapes in the
emulator too. On stage 0 a statue stands at the left in ours and
mirrored at the right in the emulator at the same moment, and nowhere in
the emulator's first round: the game places it, not the renderer (ours
sends it with the stage's own matrix).

**The DirectX 12 tracer against Vulkan's** (all 15 stages, Medium): on
twelve stages 1,143 to 6,977 pixels of 1,228,800 differ, by 1 to 3
steps. On stages 18, 19 and 20 the pictures differ widely: the water's
traced mirror, which the DirectX tracer does not have (known). A
DirectX 12 renderer does not exist: the picture itself is drawn through
Vulkan with either tracer.

**Not covered.** One pair of fighters; the first four seconds of a
fight; no hit, no throw, no ring-out, no replay, no victory pose, no
menu, no attract sequence: effects and materials that come only there
were neither counted nor compared. The fighters themselves are left out
of the comparison with the emulator (they sway, and the two are not at
one frame). The emulator is a second reading, not the board: where the
two agree both can be wrong. The ray tracers' own handling of a strip
with colors and an environment map was not changed.

## 100. After the audit: the board's frames on a floor, the statue of stage 0, the lamp everywhere (2026-11-27)

**Why.** Section 99 left "cut first or light first" open for want of a
frame of the real board, said of stage 0's statue that the game places
it without having looked at how, and had the cage's lamp checked in
three of the ways the game draws.

**Cut first or light first: the board is ours'.** The maintainer
supplied three frames from videos of the board, each from the fight's
first quarter second: the cave (stage 20, Lei-Fei against Brad), the
temple garden (stage 17, Lei-Fei against Kage) and the snow (stage 2,
Jacky against Goh). Ours and the emulator were run with the same
fighters; the camera stands still from the fight's first vertical blank
to the 111th, where the emulator's picture is taken (ours' pictures 9
and 111 after are the same on the floor away from the fighters).

- With these fighters ours and the emulator agree on the cave's floor
  and on the snow's but for a band at each edge (within 2 of 255), and
  differ all over the temple's: the emulator is 1 to 3 brighter at the
  floor's far edge and 13 to 15 at the picture's foot. So the temple's
  frame is the one that can tell.
- The frame is brought to ours' geometry by its HUD (section 93's way).
  Its colors are set by the scene itself: a straight line per channel
  over every pixel where ours and the emulator agree within 2 (12 % of
  the picture, the HUD and the fighters left out). Where the two agree
  the open question cannot matter, so the line owes nothing to it. It
  misses those pixels by 2.6 to 3.0 (spread).
- The floor between the fighters, in bands of 40 lines, areas of 17 x 17
  averaged, mean of the three channels of 255:

      lines        emulator - ours    board - ours
      620..660          +2.5              +1.6
      660..700          +5.8              -0.4
      700..740          +8.4              +3.6
      740..780          +9.2              -4.3
      780..820          +9.1              +0.9
      820..860         +12.9              +0.4
      860..900         +13.2              -2.0
      900..940         +14.0              -1.6

  The strips at the left and right edges and across the foot of the
  picture say the same: the emulator +1 to +15, the board within 4.5 of
  ours in every band (20 bands in all, their mean -0.5).
- The same measure on the two other frames, as a check of the method:
  on the cave's floor the board is within 3.4 of ours in 19 of 20 bands
  (one band beside the HUD's icons is not); on the snow within 1.3 in
  all 12, three of them bands at the far left and right where the
  emulator is 4 to 8 brighter than ours.
- So on these floors the board draws what ours draws. Section 99's
  first open item is closed and nothing is changed. (Why the emulator
  is brighter there was read from its source, cutting before lighting,
  and not proven by a test.)
- Limits: one frame of one video for the stage that tells; that the
  videos are of a real board is their makers' word (the frames have the
  arcade's one-player HUD and the stair-stepped edges of 480 lines; if
  one were an emulator's, it is not this emulator's); the color line
  rests on scene pixels up to a brightness near 130.

**The statue of stage 0: the game throws dice.** In section 99 a statue
stood at the left in ours, at the right and mirrored in the emulator,
and nowhere in the emulator's first round. The decompilation has the
reason (read, not copied): as a stage is set up, the routine that sets
up ten pieces of the ruins gives each its whole look or its broken look
by one bit of the game's random number (the second piece always whole),
and draws the one or the other from then on. The generator is seeded
once, as the machine starts, from the board's clock, and over a hundred
places draw from it (a face's blinking among them), so every start at
another time of day shows another set.

Checked in ours: the generator's state set to six values at the moment
the stage is forced, the pieces' flags read back and the picture
looked at. The left statue stands in three, the right in three (both in
one, neither in one); the stone lion in the middle is whole in four and
a white broken block in two. The eighth piece's bit goes with the right
statue in all six, the ninth's with the left, the tenth's with the
lion. The emulator's two rounds are two of these (neither statue and
the broken lion; the right statue alone). The two statues are mirror
twins on either side of the stage's middle: that was the "mirrored".
Ours' tests show one set every time because they run on a fixed clock.

**The cage's lamp in every way the game draws** (the lamp's box, red
of 255, at the audit's three moments; the lamp pulses):

    release 0.5.26, before section 99      100  102  102
    Linux, the lights on the GPU           197  169  181
    Linux, the lights on the processor     197  169  181
    Linux, the software renderer           196  167  179
    Linux, ray tracing (Vulkan), Medium    199  170  182
    Linux, ray tracing (Vulkan), High      199  170  182
    Windows (Proton), no ray tracing       197  169  181
    Windows (Proton), ray tracing, Vulkan  199  170  182
    Windows (Proton), ray tracing, DX12    199  170  182
    the emulator                           209  162  189

Section 99's "the ray tracers' own handling was not changed" needs no
follow-up: the lamp is drawn by the same strips under either tracer.

**What would settle the rest of section 99's open items.** None of them
showed in the 15 pictures, so the first step is the same for each and
needs nothing from outside: a switch in a test build that does it the
other way (the translucent list's depth test as "greater or equal"; the
sort by the nearest depth; the offset color not held before the fog; the
mip level's D above 4 taken as 4; the far plane read), every stage drawn
both ways over a whole match, and the pixels that change counted. An
item that changes nothing in this game is closed by that. For one that
does, the place it shows is then known, and a frame of the board at
that place decides, as the temple's floor did here. The shadow volume
cut by the near plane needs no board: the library's own software path
closes the hole, so closing it is right; what is missing is a moment
that shows it (the camera low and close: a throw or a replay).

**Not covered.** As section 99: one pair of fighters a stage and the
first seconds of a fight.

## 101. The DirectX 12 tracer mirrors the water too (2026-11-27)

**What was wrong.** Section 99 laid the two tracers' pictures side by
side and left three stages (18, 19 and 20) where the DirectX 12 tracer's
was not Vulkan's. The cause was known: `rt_light.hlsl`, the twin of
`rt_light.comp`, had none of the water's mirror (sections 81 and 90),
and `gpu_render.c` kept two things for the Vulkan tracer alone because
of it: the reflection passes drawn by the GPU renderer with the mirror
over them (the water's mode 2), and the pass left out with the mirror in
its place (mode 1, the Shrine). With DirectX ray tracing a water stage
got neither. The maintainer asked for the two tracers to give the same
picture.

**The change.** `rt_light.hlsl` has the water's parts now, line for line
its twin's: the three mirror fields of the constants (they were in the
buffer already: `rt_scene.c` fills one record for both), mode 3 of
`closest` (a mirror's own ray does not meet the water), `water_fog`,
`water_seen`, the water's branch of `radiance`, the mirror's block of
`main` and its sum with the glass's reflection. Where the GLSL takes one
of two values by `?:` and one of them traces rays, the HLSL has an `if`
(the twin's rule from section 75: DirectX's `?:` works both sides out,
and the rays' numbering must stay the same). `gpu_render.c`'s two
conditions lost "and the tracer is Vulkan's" (`passes_follow`, and the
call of `view_host_rt_mirror`). Nothing is DirectX's by default: ray
tracing through DirectX 12 is still the player's choice.

**Checked.** The Windows development build under Proton, every real
stage from its snapshot at 7600, ray tracing at Medium through DirectX
12 and through Vulkan, 1,228,800 pixels:

| Stage | Pixels that differ | Largest step | | Stage | Pixels that differ | Largest step |
|---|---|---|---|---|---|---|
| 0 | 6,977 | 2 | | 17 | 6,070 | 3 |
| 1 | 4,523 | 3 | | 18 | 0 | 0 |
| 2 | 4,582 | 3 | | 19 | 3,095 | 3 |
| 3 | 4,573 | 2 | | 20 | 1,823 | 2 |
| 4 | 5,795 | 3 | | 21 | 4,810 | 2 |
| 5 | 4,620 | 2 | | 22 | 5,010 | 3 |
| 11 | 1,141 | 3 | | 23 | 2,966 | 1 |
| 16 | 2,720 | 2 | | | | |

- Before, stage 18 differed in 17,731 pixels by up to 229, and stages 19
  and 20 as widely. The water stages are now like the others.
- On stage 18 ray tracing changes nothing at that moment with either
  tracer (Vulkan's picture is the picture without ray tracing, pixel for
  pixel), so that stage says only that the reflection pass is drawn
  again; stages 19, 20 and 21 have the mirror itself.
- What is left on every stage is single traced pixels scattered over the
  lit floors, a step of 1 in nearly all (stage 0: 0.6 % of the pixels
  differ at all, none by more than 2): a ray that just passes in one
  tracer and just meets something in the other. The two shaders are
  made by two compilers and the two tracers build their own acceleration
  structures; this much cannot be brought to nothing, and it is not seen.

**Not covered.** A Windows PC's own DirectX 12 driver (under Proton both
tracers end on the same Vulkan driver); the High setting; more than one
moment a stage.

## 102. Section 99's open items, each the other way, for a whole match on every stage (2026-11-27)

**Why.** Section 99 left a list of places where `view.c` and the
emulator differ and nobody knows the board's way; none showed in the 15
pictures. Section 100 said what would settle them: a test build with a
switch for each, every stage drawn both ways for a whole match, the
changed pixels counted. What changes nothing is closed; for what does,
the stage and the spot are then known, and a frame of the board there
decides.

**How.** Test code only (not in the repository; the patch scripts are
kept beside the test's results): the software renderer, for every
picture of the screen, draws the scene again with one thing the other
way and counts the pixels that differ from the picture it shows. A
seventh "switch" that changes nothing checks that a second drawing is
the first (0 pixels in every frame). Each real stage from the audit's
snapshot (Jacky against Brad), 4,000 vertical blanks past the fight's
first: the stage's and the fighters' intros, a match of two rounds with
both players' buttons pressed by a fixed pattern while the fight runs
(hits, throws, two knockouts), the replays and the victory pose, which
nothing presses through, and the first 16 seconds of the match that
follows on another stage. 68,379 pictures, about 4,560 a stage.

**Closed: nothing changes, on any stage, in any picture.**

- *The far plane.* The matrix command's far word is 1000 in every one of
  the 8.6 million commands, and no strip of 50 million lies wholly
  beyond it.
- *The background plane's depth.* The plane's own depth and the depth
  register are both 0.00001; with the depth starting there and not
  empty, no pixel differs.
- *A shadow volume the near plane cuts.* It happens often (in 700 to
  1,100 pictures of a match on six stages: replays and poses bring the
  camera close), and the remedy is known (the library's own software
  path closes the hole). Built as a switch: the two points where each
  cut triangle meets the plane are an edge of the hole, and a fan of
  triangles over those edges, in the plane, closes it. No pixel differs.
  The fans show why: the holes lie thousands of pixels beside or below
  the screen (the first of the snow's: columns -8,328 to -6,737, lines
  7,600 to 9,523). The near plane cuts these volumes where the picture
  is not. So on all 15 stages: not one of the 68,379 pictures has a pixel that closing the holes changes.

**It changes pixels; which way is the board's is not shown.**

- *The translucent list's depth test, "greater" as sent or "greater or
  equal".* On 14 stages: 6,399 pixels in 4,112 of 63,834 pictures, one
  or two a picture. On stage 11: 9.9 million, in half its pictures,
  11,823 in the worst. A probe of one such pixel shows what it is. The
  corridor's floor is sent twice with the same corners: in the opaque
  list with the filter word 2, and in the translucent list with the
  filter word 3 and a one-plus-one blend, both with "greater". Those
  are the chip's two passes of a tri-linear filter (the two mip levels a
  pixel lies between, each by its share, added). With "greater" the
  second pass loses to the first at the same depth, so `view.c` shows
  the first pass alone and in full: one mip level, not two blended. The
  floor's far part at one moment of the fight: 103 91 77 here (both
  renderers), 148 140 124 with the switch (both passes in full: too
  much), 126 111 92 in the emulator (which takes "greater or equal" for
  a sorted list whatever is sent, and weighs the passes by two bits of
  the D number). The game sends a second pass that "greater" could
  never let through, and its makers saw their floor on the board, so
  "greater or equal" is very likely the chip's way in a sorted list;
  but it is only right together with the two passes' shares, which
  nothing here works out yet. Left as it is. A frame of the board on
  stage 11 as a round begins, the far floor beside the rails, would say
  how bright that floor is.
- *The texture's color plus the offset color: held to full before the
  fog, or only at the end.* 9,229 pictures. Most on stages 0, 4 and 22
  (3,081, 2,001 and 1,952 pictures: through the fights). On stage 0,
  looked at: the fighters' whitest parts (hand wraps, a fur collar),
  where light over full has gone into the offset color (section 98) and
  the stage has fog: about 1,100 pixels, up to 19 of 255. On the other
  stages 39 to 672 pictures, up to 61 of 255; on stage 17, looked at,
  the first 92 of its 199 are the second and a half in which the stage's
  picture fades in from white. Held first, a white part is a little under white (the
  fog's share of it); held at the end it is full white. Left as it is.
  A frame of the board with a fighter's white cloth in a fight on stage
  0, 4 or 22 would say.
- *A mip-mapped texture's D number above 4: a factor (2.5 for 10), or
  taken as 4.* Stages 20, 22, 23 and 3: 23,873 pixels a picture on stage
  20 (D is 5 on the cave's rock), 16,638 on stage 22 (D is 10), 1,936 on
  stage 23, 78 on stage 3. As a factor those textures are softer. The
  emulator's own source calls its table "a rough approximation" that is
  "only used to increase" the level of detail. The maintainer's frame of
  the board in the cave (section 100) was held against both: the fine
  detail (brightness less its mean over 5 x 5) in blocks of 16 x 16,
  with a line through the blocks that do not change for what the video
  keeps of ours' detail (0.21 + 0.69 x, missing them by 0.60). Of the 20
  blocks that change the board is nearer the line's value for "taken as
  4" in 13 and for the factor in 7 (10.44 against 10.21 and 9.75): a
  lean toward "taken as 4" that is smaller than the line's own misses.
  Not decided, and left as it is. Stage 22 would say much more clearly
  (its factor is 2.5, the cave's 1.25): a frame of the board there as a
  round begins.

**The order of translucent triangles: the chip's own way is known, and
is neither.** Section 99 had the emulator sort by a triangle's nearest
corner. Read again: it sorts by the farthest (the smallest 1/w of the
three), where `view.c` takes the mean of the three. The chip does
neither: in a sorted list (and the feed register says this game's are
sorted) it orders the translucent layers of each pixel by themselves.
That was built as a test too: the translucent list drawn a layer of
each pixel at a time, from the back, by depth and then by the
triangle's number (up to 55 layers in a pixel). Held against it over
the same 68,379 pictures:

    the triangles sorted by            pixels that differ    a picture with any
    the mean of their corners (ours)       69.9 million            1,129
    their farthest corner (emulator)       89.3 million            1,447
    their nearest corner                  102.7 million            1,660

The mean is the nearest of the three to the chip's order on every
stage, and it is still not that order in nine pictures of ten: about
0.4 % of a picture, by steps up to the full range. On the stage looked
at (17) it is the fighters' hair, which is layers of translucent
strands, and a few leaves. Getting it right is each pixel's own order
in the renderers (for the GPU renderer: its translucent list drawn in
layers), a feature of its own. Not done.

**Not covered.** One pair of fighters, one match a stage; the software
renderer only (the GPU renderer draws by the same rules, section 99's
check, but its depths are its own); passes into textures were drawn as
the game is.

## 103. The GPU renderer through DirectX 12, the player's choice (2026-11-27)

**Why.** Issue 3's picture that freezes in full screen has its cause
named by Windows itself on the reporter's PC: at each freeze the event
log has the Intel graphics driver stopping and being started again
(event 4101), while the game drew through Vulkan. The GPU renderer was
Vulkan's alone; the only other way was the software picture. Whether
DirectX would spare that driver is not known. It is the way that
driver's maker has had far more use on, and trying it is how to find
out. The maintainer's rule for it: never a default for anything; the
player turns it on.

**What it is.** Not a second renderer. `gpu_render.c` draws through
SDL's GPU interface, which has a Direct3D 12 device beside its Vulkan
one; the renderer refused that device only because its shaders existed
as SPIR-V alone. Now each of its twelve shaders has a twin in DirectX's
shader language (`scene.vert.hlsl` and so on, beside the GLSL one it is
named after), compiled to DXIL into `dx_shaders.h` by
`tools/make_shaders.py`, and `shader()` hands a device the kind it
takes. The Windows build carries both; the Linux build has no DXIL and
still refuses a device that takes no SPIR-V (the native Linux game never
uses DirectX).

What SDL's Direct3D 12 device asks of a shader, read in its source: a
vertex shader's textures and buffers are in register space 0 and its
constants in space 1, a pixel shader's in spaces 2 and 3, sampled
textures before buffers; a buffer the shader only reads is a raw buffer
(`ByteAddressBuffer`: the light sets, the strips' records, the states
and the texels are read word by word, where the GLSL has arrays); every
vertex input's semantic is `TEXCOORD` with its location's number.

**The same picture.** The first pictures through Direct3D 12 differed
from Vulkan's in up to 673 of 1,228,800 pixels a stage (none on one
stage, 26 at least on the others), single pixels, by up to 105. Three
causes were tried, each by building it and counting again:

- The position's sum done as written (`precise`, tried in both
  languages): no pixel changed. Not it. (It stays in the twin's
  position; the GLSL is as it was.)
- Every sum as written (dxc's strict mode, `-Gis`): 2 pixels changed.
  Not it. (Kept: a twin should not be free to reorder what its twin
  does in order.)
- The screen-space derivative of the texture coordinates, which chooses
  a mip-mapped texture's level. GLSL's `dFdx` leaves its kind to the
  driver; HLSL's `ddx` is the coarse kind, one difference for a block
  of four pixels. With the fine kind asked for by name in the twin
  (`ddx_fine`), the pictures were the same. The GLSL asks by name too
  now (`dFdxFine`), so that a Vulkan driver whose own choice is the
  coarse kind draws what the others draw; on the driver all of this
  was made with, that changes no pixel on any stage.

After that, the Windows development build under Proton, each stage from
its snapshot at 7600, Direct3D 12 against Vulkan: the same to the pixel
on eleven stages; 1 pixel on stages 2, 4 and 22 and 2 on stage 21, a
step of 1 of 255. The other paths of the renderer, the same way:

| With | Stages | Pixels that differ |
|---|---|---|
| Ambient occlusion | 17, 22 | 0, 0 |
| Ray tracing through DirectX 12, Medium | 3, 21 | 2, 5 (a step of 1) |
| Ray tracing through Vulkan, Medium | 3, 21 | 2, 5 (a step of 1) |
| The lights on the processor | 17, 22 | 0, 0 |
| Internal resolution 1 | 17, 21 | 0, 0 |
| Internal resolution 4 | 17 | 0 of 4,915,200 |
| The wide picture (16:9) | 21, 18 | 2 (a step of 1), 0 |
| The software renderer's picture, shown through each | 17 | 0 |

The passes into textures (a stage's reflection) are drawn through
Direct3D 12 as through Vulkan (stages 18 to 21 above). Either tracer
works with either picture: both make a device of their own and bring
their result back.

**The player's choice.** `graphics = vulkan | directx` in settings.txt,
vulkan by default. In the menu and the launcher it is Renderer's third
value where the game can use it: GPU (Vulkan), GPU (DirectX 12),
Software; a game built for Linux has the two it had, and takes a file's
`directx` as vulkan (as it takes `raytracing = directx` as off).
Nothing chooses DirectX 12 by itself: not a failure of Vulkan, not the
graphics chip's maker, not the launcher.

*One way a run.* The game takes the setting as it starts and keeps it
until it ends: a window that has shown one of the two is not handed to
the other while the game runs. Under Proton that works; on Windows a
window's second swap chain of another kind can be refused, and that
cannot be tried here, so it is not relied on. A change between Vulkan
and DirectX 12 in the menu is therefore marked as the settings that
wait for the next start are; GPU or Software still changes at once.
With DirectX 12 the whole run is shown through that one device, the
software picture too.

*Which chip.* SDL asks Windows for the adapter it ranks as high
performance (with two graphics chips in a PC, the separate one); the
game has no say in it. The GPU row,
whose list is Vulkan's, rests while DirectX 12 draws, and says so.

*When it cannot start* (no Direct3D 12 device, the window not shown, or
the GPU renderer not starting on it at the run's first try): the log
says which, the run goes on in Vulkan's own order as if the setting
were vulkan, the setting is kept, and the menu's Renderer row says that
DirectX 12 could not start. `--no-direct3d12` (for tests) makes the
device fail on purpose: the run then drew through Vulkan, its picture
Vulkan's to the pixel, and the file still said directx.

*The menu*, driven by keys under Proton from a run begun each way: the
row goes through its three values; Vulkan to DirectX 12 (or back) gets
the mark and the note of a change that waits, with no renderer started
again; Software and back start the renderer again on the run's own
device (the log's driver line says direct3d12 in the one run and vulkan
in the other throughout).

**A run at the game's own pace**, three minutes on the cave (stage 20, whose water is a pass into a texture every frame), under Proton. Through Direct3D 12: 10,600 pictures shown with 10,782 presents in 179.7 s (60.0 a second), 2 of the 10,033 frames in the log's ten-second counts never shown, 7,939 reflection passes, a clean exit, no error in the log. Through Vulkan: the same but for one picture more, and 1 frame never shown. The two runs' pictures at the end are the same to the pixel. (This paragraph first said that no frame was left unshown: the script that summed the log's lines read the wrong word of them.) In both runs the pacing left "each frame put on a refresh" for the two seconds its measure takes to settle again: both at the start, Vulkan's once more in the middle with nothing scripted, Direct3D 12's once more at the end, as the test's picture was saved.

**The game built for Linux**, with all of this in: its pictures on three
stages (1, 17, 22) are what they were, to the pixel; with `graphics =
directx` in its file it says so in the log and draws through Vulkan.

**`--gpu direct3d12`** still names SDL's device for one run, whatever
the setting says; with it the GPU renderer now starts on that device
too (before: the software picture).

**The launcher's Settings page** was not looked at when this was
written ("the same row, made by the same code as the menu's"). Looked
at for release 0.5.28, the Windows launcher under Wine: the row has its
three values and saves them (`renderer` and `graphics`); the GPU row
rests under DirectX 12 with its note; a Linux launcher whose game is
the native one has the two values, and writes a file's `directx` back
as vulkan. And it showed a fault the menu could not: the row's text,
written to the menu's panel, was 15 lines in the launcher's, which
holds 11, and ran out of the panel over the page's foot. The text is
now as long as the two-value one: 8 lines, which leave the launcher's
panel room for a note of three. That a change between the two waits
for the next start is no longer in it; the menu's mark and note say so
when the change is made.

**Not covered.** Windows itself, and any graphics chip but the
development PC's GeForce: under Proton, Direct3D 12 ends on the same
Vulkan driver, so nothing here says what Intel's DirectX driver does
with it, which is the question it was made for. Fullscreen, Exclusive
and the screen's modes through Direct3D 12; a two-PC game; a Steam
Deck; how fast it is beside Vulkan.
