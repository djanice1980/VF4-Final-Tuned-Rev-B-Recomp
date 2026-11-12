# runtime/machine: VF4debug's machine

These files are VF4debug's `src/` (private repository
`djanice1980/VF4debug`, MIT license, same maintainer) as of commit
`c3b2abb` (2026-11-12), all of it but `main.c` (the recomp's
`runtime/headless/main.c` is that file, changed). Copyright (c) 2026 The
VF4debug contributors, under the MIT license: see VF4debug's `LICENSE`.

Changes for the recomp are marked `recomp:` in the code. To carry a later
VF4debug fix over:

```bash
diff -ru "../VF4debug/src" runtime/machine
```

and apply what VF4debug changed, keeping the `recomp:` changes.

What the recomp changed:

- `machine.c`: `events_fire` is no longer `static` (the native runtime,
  `runtime/native/rt_native.c`, fires events between blocks of translated
  code).
- `mem.h`, `mem.c`: a second page table for stores, `mem_wpage`, which is
  `mem_page` with the pages that hold translated code (`mem_code_page`)
  left out; a store there goes to `mem_code_write`, which shows the new
  bytes to `mem_code_store` before making them (the native runtime's
  changed-code watch, docs/design.md "Code the game changes"). The host
  accessors (`g_w*`, `g_write`) show their bytes too. `mem_wpage_sync` runs
  after every change to `mem_page`: in `mem.c`, at the end of `hw_init`'s
  low-RAM loop (`hw.c`) and of `hw_sound_init`'s (`hw_sound.c`). With no
  code page marked (the interpreter, `--interp`) nothing behaves differently.
- `observe.h` (the observed stores) and `sh4_exec.inc` (the store-queue
  flush): call `mem_code_note` before writing.
- `machine.h`, `machine.c` (M3): `machine_host_frame`, a hook the game
  window (`runtime/host/game_machine.c`) sets. `vblank_event` calls it after
  the controller's frame step; it is NULL in the headless program.
- `view.h`, `view.c` (M3): a host mode (`view_host_start`) that draws every
  frame with no files and no pipe, and `view_host_picture`, the newest
  picture (`VIEW_W` x `VIEW_H` RGB). Windows: the CPU count from winpthreads,
  `_mkdir`, no `SIGPIPE`.
- `mem.c` (M3, Windows only): each RAM area is one `VirtualAlloc` with its
  guard after it. The mirrors and aliases are in the page table only, with
  no host mappings at guest addresses.
- `hw_cart.c` (M3, Windows only): the DIMM image as a copy-on-write file
  view (`PAGE_WRITECOPY`), as the private `mmap` on Linux.
- `machine.c` (M3, Windows only): `snapshot_save_mem` goes through a
  temporary file, since Windows has no `open_memstream`.
- `hw.h`, `hw_sound.c` (M4): `hw_sound_chip` puts the sound chip model
  (`runtime/sound/aica.c`, the recomp's own code) on the sound registers in
  place of the mailbox model. The chip runs 32 samples at a time on a
  machine event, raises the system chip's external interrupt bit 1, and is
  kept in the snapshots. `hw_sound_arm_budget` sets its ARM7's instructions
  a sample. With `hw_sound_chip` 0 (the default) nothing behaves
  differently.
- `runtime/headless/main.c` (M4): `--sound chip|mailbox`, `--arm-budget N`
  and `--wav FILE`.
