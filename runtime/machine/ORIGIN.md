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
