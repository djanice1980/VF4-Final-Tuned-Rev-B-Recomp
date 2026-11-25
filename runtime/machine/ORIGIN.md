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
- `hw.h`, `hw_card.c` (the game menu's card editor): `hw_card_last`, the
  card in a unit or the one that left it last, as it left (a card taken
  while the lock was on leaves as the lock opens, right after the game's
  last write), and `hw_card_held`, whether a game holds the card: from a
  read with the lock off (the game's read at the start of a game, which it
  writes back before it locks the unit) or while the lock is on, until the
  lock opens or the card leaves (`read_open` in the unit's state).
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
- `view.h`, `view.c` (2026-11-13): the wide screen, `view_host_wide`. A
  pass to the whole of the game's 640 x 480 screen is drawn wider (up to
  `VIEW_W_MAX`, 1,280); after transform, lighting and the sort,
  `scene_shift` moves its triangles, volumes and clip rectangles right by
  half the extra width (a clip rectangle that reaches the screen's edge
  reaches the target's), and `bg_plane` moves the background plane with
  them. `keep_left` keeps triangles and volumes that lie left of the
  game's screen. The buffers are `FBW` (1,280) wide; `PASS_W` (1,024) is
  the old `FBW` wherever it bounded a pass. `view_host_picture` gives the
  picture's width. Without `view_host_wide` nothing behaves differently
  (docs/design.md section 39).
- `sh4.c`, `sh4_exec.inc` (2026-11-14): FSCA takes its sine and cosine
  from `runtime/cpu/fsca.h`'s table instead of the host's `sin` and `cos`,
  so two PCs playing one game get the same bits whatever their math
  library. The values are the ones `sin` and `cos` gave on Linux, so runs
  keep their hashes (docs/design.md section 45).
- Rollback (2026-11-16, docs/design.md section 55). `mem_track.h`,
  `rollback.h` and `rollback.c` are the recomp's own files. `mem.h`,
  `mem.c`: the pages a frame writes are tracked: a page armed for
  tracking is NULL in `mem_wpage` (`mem_wpage_sync` leaves armed pages
  out too), so its first store takes the slow path, which calls
  `mem_track` (`mem_code_write`, and `HOST_STORE` for `g_w*` and
  `g_write`). `mem_track` is also called by `ram_slow_write` (`hw.c`) and
  the store-queue flush (`sh4_exec.inc`); `mem_track_host`, before device
  code writes through a host pointer, by the sound driver's block move
  (`hw_sound.c`), `target_store` (`view.c`) and a snapshot's pages
  (`machine.c`). `machine.h`, `machine.c`: `snapshot_dev_size`,
  `snapshot_dev_save`, `snapshot_dev_load` and `snapshot_dev_part`, what a
  snapshot holds but the memory as one buffer. With rollback off
  (`mem_tracking` 0) nothing behaves differently.
- Frames run again (2026-11-16, docs/design.md section 56). `view.h`,
  `view.c`: `view_rerun` and its three modes; `lists_begin` does not take
  a screen's lists in `VIEW_RERUN`, `render_started` draws no screen
  unless `VIEW_LIVE`, and going back makes the texture cache look at its
  textures again (`gen`). With `VIEW_LIVE`, the only mode outside
  rollback's replays, nothing behaves differently.
- The light that goes to the alpha (2026-11-23, docs/design.md section 64,
  issue 2). `view.c`: the light set's header no longer puts the lights
  out (it only says which are on: the game's extra light is sent before
  its header and was never on); a light keeps how its amount is made, its
  routing and a spot light's cone (`light.dmode`, `route`, `ck`, `cb`);
  and a light routed to the alpha (4, 5, 12, 13) adds to or takes from a
  vertex's alpha and leaves its colour alone. This changes VF4debug's own
  picture too and should go back to it.
- Wide screens, a pass into a texture (2026-11-23, docs/design.md section
  65). `view.c`: on a wide screen such a pass is drawn wider too
  (`scene_draw`, `scene_shift` with the pass's own width), its own part
  stored as before (`target_store` with the columns to skip), the wide
  picture kept (`wide_tex_keep`) and used by the screen pass's polygons
  that have the texture (`wide_tex_use`, `rstate.wide_u`). With no wide
  screen (VF4debug has none) nothing behaves differently.
- Lighting on the GPU (2026-11-24, docs/design.md section 67). `view.h`,
  `view.c`: `view_host_gpu_lighting`; with it on, `strip_job` leaves the
  lights of a strip the GPU can light to the host (the colours carry the
  normal and the point lit at, `dtri.gl` the strip), and `gpu_export`
  hands over a record for each strip and the light sets
  (`view_gpu_frame.lit`, `.lights`; `view_gpu_vertex.n`, `.l`, `.lit`).
  Off (the default, and always without the host's GPU path) nothing
  behaves differently.
- The view planes for a wide picture (2026-11-24, docs/design.md section
  68). `hle_cull.c` is the recomp's own file: a hook on `proj_from_vfov`
  and `proj_from_hfov` that sets the left and right planes of
  `view_side_planes` wider for a wide picture (`hle_cull_wide`). `hle.h`
  declares it and `hle.c` installs it. At 640, the default (VF4debug has
  no wide picture), the hook does nothing. `runtime/headless/main.c` has
  `--cull-wide W` for it.
- Less work on the machine's thread, and frame skip (2026-11-24,
  docs/design.md section 71). `hw.h`, `hw.c`: `hw_access_notes`; 0 (the
  game sets it) keeps only the device accesses no model answers out of
  `note_access`'s table; 1, the default, is VF4debug's way. `view.c`:
  `order_sort`, the translucent triangles' order by a radix sort that
  gives `by_depth`'s order (this could go back to VF4debug);
  `view_host_every` (`view.h`), the game window's draw-every-nth-frame,
  which uses VF4debug's own `every`.
- Ambient occlusion's needs (2026-11-24, docs/design.md section 73).
  `view.h`, `view.c`: `gpu_export` says of each batch it hands over
  whether it is a model's strips or what came straight to the chips
  (`view_gpu_draw.solid`), and hands over the first strip's projection
  (`view_gpu_frame.proj`). The host's GPU path alone reads them; nothing
  drawn here changes.
- The scene for ray tracing (2026-11-24, docs/design.md section 74).
  `view.h`, `view.c`: `view_host_rt`; with it on, `gpu_export` also hands
  over every strip's triangles as `strip_job` would walk them, without the
  near plane, the culling and the screen's edges (`rt_export`:
  `view_gpu_frame.rt_tris`, `.rt_strips`, `.rt_near`), and the light sets
  whatever lights the vertices. Off (the default, and always without the
  host's GPU path) nothing is done and nothing behaves differently.
- Passes into textures drawn by the host (2026-11-25, docs/design.md
  section 78). `view.h`, `view.c`: `view_host_gpu_passes`; with it on (and
  the host's GPU path), `scene_draw` hands a pass into a texture of the
  kind the game makes to `gpu_export` instead of rasterizing it
  (`view_gpu_frame.target`, `.target_ox`; `gpu_target_take`), does not
  call `target_store`, and marks the states of later passes whose texture
  is that place (`gpu_target_use`: `view_gpu_state.target`,
  `.target_ox`). Off (the default, and always without the host's GPU
  path) those passes are drawn here as ever. `lists_begin`: in the game
  window (`host_mode`) a pass's lists are taken when the lists of the
  screen before it were (frame skip, a frame run again); the headless
  program's rule is VF4debug's, unchanged.
- `runtime/headless/main.c` (M4): `--sound chip|mailbox`, `--arm-budget N`
  and `--wav FILE`. (M6) It keeps the power-on state for test mode
  (`rt_native_power_on`) and reports the program switches.
