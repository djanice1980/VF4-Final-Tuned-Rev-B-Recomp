# Vulkan headers (Khronos)

The ray tracing (runtime/host/rt_vk.c, docs/design.md section 74) calls
Vulkan itself, and needs Vulkan's declarations to be compiled. The pinned
Windows compiler (llvm-mingw) and SDL3's development files have none, so
Khronos's own headers are here, and the builder compiles with them on every
system (they come before any the system has).

- What: `vulkan/vulkan_core.h`, `vulkan/vk_platform.h` and the `vk_video/`
  headers the first one includes. Header version 282 (Vulkan 1.3).
- From: the copy in SDL 3.4.20's source, `src/video/khronos/` (SDL has them
  from https://github.com/KhronosGroup/Vulkan-Headers).
- Copied as they are, none changed.
- License: Apache-2.0 (each file's `SPDX-License-Identifier`); the text is
  `LICENSE-Apache-2.0.txt` here. Copyright 2015-2024 The Khronos Group Inc.

Only declarations are used: the game links no Vulkan library, and gets
Vulkan's functions at run time through SDL's loader.
