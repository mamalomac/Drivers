# VKOn11

An experimental **Vulkan 1.0 driver for graphics cards that only have a Direct3D 11 driver**,
written for the AMD Radeon HD 6570 (1 GB) and other Radeon HD 5000/6000-series cards that AMD
never gave Vulkan support.

It is a user-mode Vulkan driver (an "ICD"). It translates Vulkan calls into Direct3D 11 calls,
and AMD's existing driver then runs them on the GPU. Nothing in the Windows kernel is replaced,
no test-signing mode is needed, and uninstalling is one registry value.

```
Vulkan application
      │  Vulkan 1.0
      ▼
  vkon11.dll        SPIR-V → HLSL → D3D bytecode, state translation, memory emulation
      │  Direct3D 11 (feature level 11_0)
      ▼
AMD Radeon driver → HD 6570
```

## What this is not

Read this before installing, so you are not disappointed:

- **Not DirectX 12.** There is no DX12 support in this repository. See [DirectX 12](#directx-12).
- **Not the latest Vulkan.** It reports Vulkan 1.0 with a subset of features. Programs that
  require Vulkan 1.1 or newer (most recent games, DXVK, vkd3d-proton) will refuse to start.
- **Not a speed-up for existing games.** DirectX 9/10/11 and OpenGL games do not go through this
  driver at all and run exactly as before. Vulkan programs run through an extra translation
  layer, so the best case is "roughly what the same scene costs in Direct3D 11", never more.
  The only thing you gain is that some Vulkan-only programs can start at all.
- **Not tested on a real HD 6570.** See [Test status](#test-status).

## Install (Windows 10/11, 64-bit)

1. Install AMD's last driver for the card (Catalyst 15.7.1 or Crimson 16.2.1 Beta) if it is not
   installed already. The card must work in Direct3D 11.
2. Install the Vulkan loader if you do not have one: get the **Runtime Installer** for Windows
   from <https://vulkan.lunarg.com/sdk/home>. Cards without a Vulkan driver do not ship it.
3. Copy the `dist` folder somewhere permanent, for example `C:\VKOn11`.
4. Double-click `selftest.bat`. It renders test scenes through the driver on your card and
   prints `SELF-TEST PASSED` or the checks that failed. This works before installing.
5. Right-click `install.bat` → **Run as administrator**.

Vulkan programs now list your card as `AMD Radeon HD 6570 (VKOn11)`.
To remove it, run `uninstall.bat` as administrator and delete the folder.

Only 64-bit programs are supported; there is no 32-bit build.

## What works

**Covered by the self-test (passing):**

- Device setup, memory allocation and mapping, host-visible and device-local buffers
- Vertex, index, uniform (at an offset) and indirect buffers, including re-upload after the
  application writes to mapped memory
- 2D colour and depth images, texture upload, nearest sampling, 4x MSAA with resolve
- Render passes with whole and partial clears, dynamic viewport and scissor
- Vertex + fragment shaders, push constants, combined image samplers
- Depth test, back-face culling with both winding orders
- Draw, indexed draw, indexed indirect draw, instancing with per-instance attributes,
  non-zero `firstVertex` / `firstInstance`
- Buffer and image copies in every direction, blit, clear-image, fill/update buffer
- Window output through `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`
- Negative viewport height (`VK_KHR_maintenance1`)

**Implemented but not exercised by any test yet (expect bugs):**

- 1D/3D/cube/array images, mipmapped and BC1–BC7 textures, anisotropic and compare samplers
- Blending (including dual-source and per-attachment), stencil, depth bias, wireframe
- Dynamic uniform buffers, separate samplers and images, specialization constants
- Multiple subpasses, secondary command buffers, multi-draw-indirect, `vkCmdResolveImage`
- Occlusion queries
- Compute shaders with storage buffers and storage images, `gl_InstanceIndex`
  (written, but the shader compiler available in the test environment cannot build them)

**Not implemented:**

- Geometry and tessellation shaders
- Storage buffers/images in vertex or fragment shaders
- Input attachments, texel buffers, timestamp and pipeline-statistics queries
- Linear-tiling (mappable) images, sparse resources, triangle fans, wide lines, large points
- Everything in Vulkan 1.1 and later (multiview and so on)

Known approximations: front and back stencil masks/references share one value, component
swizzles on image views are ignored, and depth images cannot be uploaded from a buffer.

## Test status

The self-test (`tests/selftest.cpp`) renders off-screen and compares pixels. It was run here
with the Windows build under Wine, where Direct3D 11 is itself emulated on a software renderer:
**32 checks pass, 0 fail, 3 tests skipped.** The three skipped tests (`gl_InstanceIndex`, and both
compute tests) need HLSL features that Wine's built-in shader compiler lacks; they should run on
real Windows, where Microsoft's compiler is used. The GitHub Actions workflow builds with MSVC
and runs the full self-test on Microsoft's software Direct3D adapter.

It has **not** been run on an HD 6570 or any other real GPU. Expect bugs. If `selftest.bat`
reports failures on your card, the output lines are the bug report.

## Performance notes

- Command buffers are recorded cheaply and replayed on the Direct3D 11 immediate context at
  submit time, so all Direct3D work happens on the thread that calls `vkQueueSubmit`.
- Buffer contents live in system memory and only pages the application actually wrote are
  re-uploaded (tracked with the OS write-watch facility).
- Compiled shaders are cached in `%LOCALAPPDATA%\VKOn11\shadercache`, so each shader is compiled
  once, not on every launch. The first run of a program will stutter while shaders compile.
- `selftest.bat --bench` prints draw calls per second for your machine.

## Settings (environment variables)

| Variable | Effect |
| --- | --- |
| `VKON11_LOG=1` or `=C:\path\log.txt` | print driver messages (unsupported features, shader errors) |
| `VKON11_DUMP_HLSL=1` | with logging on, print the generated HLSL of shaders that fail to compile |
| `VKON11_ADAPTER=n` | expose only DXGI adapter number `n` |
| `VKON11_SHADER_CACHE=0` or `=C:\dir` | disable the shader cache, or move it |
| `VKON11_D3D_DEBUG=1` | create the Direct3D device with the debug layer |

## DirectX 12

Not included, and full DirectX 12 is out of reach for this hardware by any route:

- A native DX12 driver would need a new signed kernel driver, and the HD 6570 lacks things
  DX12's driver model requires (per-process GPU virtual memory, for one).
- The translation route (DX12 → Vulkan via vkd3d-proton) needs Vulkan 1.3 with descriptor
  indexing and buffer device addresses. The chip has a fixed number of texture and buffer slots
  per shader stage, so those cannot be provided on top of Direct3D 11.

A partial "DX12 feature level 11_0 on top of D3D11" layer that runs simple DX12 programs is
conceivable as a separate project; nothing here implements it.

If you only need DX12 or Vulkan 1.3 programs to *start* and speed does not matter, Windows 10/11
already include a CPU-based DX12 adapter ("Microsoft Basic Render Driver"), and Mesa's `lavapipe`
is a CPU-based Vulkan 1.3 driver. Both render on the processor, not the HD 6570.

## Building

Windows (Visual Studio 2019+ with CMake):

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Linux cross-compile (`g++-mingw-w64-x86-64`, `cmake`, `ninja-build`):

```
./scripts/build-dist.sh
```

SPIRV-Cross and the Vulkan headers are downloaded automatically at configure time.
`scripts/compile-shaders.sh` regenerates the test shaders (needs `glslangValidator`).

## Source layout

| File | Contents |
| --- | --- |
| `src/icd.cpp` | Vulkan loader interface, entry-point table |
| `src/instance.cpp` | adapters, features, limits, format support |
| `src/device.cpp` | device, memory shadowing, buffers, images, views, samplers, queries |
| `src/shader.cpp` | SPIR-V → HLSL → bytecode, stage-interface matching, shader cache |
| `src/pipeline.cpp` | descriptor sets, layouts, render passes, pipelines |
| `src/cmd.cpp` | command recording and replay, draws, transfers, clears |
| `src/swapchain.cpp` | window surfaces and presentation |
| `src/formats.cpp` | Vulkan ↔ DXGI format table |
