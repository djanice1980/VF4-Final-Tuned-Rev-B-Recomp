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
  differently. (Until M6 the chip's state was registered with size 0, so no
  snapshot held it: `n` was read before `aica_state(&n)` set it, in the
  same call's arguments. Found when the test program's restart left the
  game's ARM running in emptied sound RAM.)
- `hw.h`, `hw_cart.c` (M5): `hw_cart_init_memory`, the DIMM image already
  in memory. The installed game keeps the disc's encrypted file and decrypts
  it at start (`runtime/host/dimm_image.c`); `hw_cart_init` and its file
  mapping are unchanged for development builds.
- `hw.h`, `hw_card.c` (M6): `hw_card_put` and `hw_card_get`, a card in
  from bytes and the card in a unit out as bytes, for the game window's
  card files (`runtime/host/game_machine.c`). The same as the controller's
  `card pN insert` and `card pN save`, without files.
- `hw.h`, `hw_card.c` (card profiles): `hw_card_take`, the player takes the
  card out, as the controller's `card pN eject` (a unit whose lock is on
  holds the card until the game opens it), for the game menu's Player 1
  card and Player 2 card.
- `view.h`, `view.c` (M6): the GPU path, `view_host_gpu`. A pass to the
  screen is handed to the host after transform, lighting and the
  translucent sort (`gpu_export`: the triangles in raster()'s order with
  their states, the opaque list's volumes, the registers, the background
  plane, the textures through an arena of texels) instead of being
  rasterized; passes into textures are still drawn here. `target_clear`'s
  plane is computed by `bg_plane`, which both use. Without
  `view_host_gpu` nothing behaves differently.
- Test mode (M6): the machine runs either program, the game (main.bin) or
  its test program (test.bin), and switches at the BIOS's service 0x44
  (docs/design.md section 20).
  - `tables.h`, `dispatch.c`: `gen_test_routines`, `gen_test_globals` and
    their counts (the test program's tables, `tables_test.c`);
    `disp_program` and `prog_routines`, `prog_globals` and their counts, the
    running program's. `disp_init` takes its tables from `disp_program`, can
    run again (it frees the old tables) and clears `disp_exc_depth`;
    `global_text` names the running program's globals.
  - `control.c`: a routine's spec comes from `prog_routines`.
  - `hle.h`, `hle_boot.c`: `hle_program`, which service 0x44 calls with the
    other program when it is set (unset, the service stops the machine as
    before). `hle_vectors_write` takes the exception entry as
    `irq_entry_via_firmware` - 0x44 when the program's table has no
    exception entry (the test program's is unnamed; the bytes are main's).
    `hle_boot_install` can run again: the snapshot blob and the BIOS and
    backup RAM areas are added the first time only, and the low-RAM words
    are left (the restored power-on state has them).
  - `hw.h`, `hw_cart.c`: `hw_cart_image`, the image's bytes, for loading the
    test program from its Naomi header table (`runtime/native/rt_native.c`).
  - `observe.h`, `observe.c`: `obs_program_changed`, which clears the
    game's mode words (the test program has none).
  - `view.h`, `view.c`: `view_program_changed`, which makes the view look
    up its tables again (the function-local `done` became the file's
    `tables_done`).
- `hw_dimm_net.c`, `hw_cart.c` (2026-11-13): the top 16 MiB of the board's
  memory (the network model's window, 0x0F000000 on) is there whether the
  network side is on or off; `in_window` no longer asks `ns.on`. The game
  keeps its network-check log there, and without it the boot's check
  screen showed -1s and a stray glyph (docs/design.md section 32). With
  the network side off the registers still answer nothing.
- `view.c` (2026-11-13): the guard band (`guard_dist`, `guard_cut`,
  `tri_guard`, `beyond_guard`). A triangle whose screen corners reach past
  a million pixels (cut at the near plane close to the eye) was left out by
  `tri_fill` and `vol_tri`; it is now cut in view space at four planes
  4,096 pixels outside the target, as the near plane cuts it, so that what
  lies on the screen is drawn. A strip's room for triangles keeps two per
  triangle and adds a spare the cut pieces share (`tri_spare`, `tri_room`).
  Triangles within the bound are drawn as before (docs/design.md section
  38).
- `runtime/headless/main.c` (M4): `--sound chip|mailbox`, `--arm-budget N`
  and `--wav FILE`. (M6) It keeps the power-on state for test mode
  (`rt_native_power_on`) and reports the program switches.
