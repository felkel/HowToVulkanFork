# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A personal fork of `SaschaWillems/HowToVulkan` (origin is `git@github.com:felkel/HowToVulkanFork.git`). Upstream is a minimalist single-page tutorial that teaches rasterization with Vulkan; the whole renderer lives in one `main()` in `source/main.cpp`, deliberately without abstraction layers, helper classes, or error-handling frameworks.

The fork's own commits ("My comments added to the forked repo", "Semaphore renaming + .presentMode test") add study annotations and experiments on top of that. Keep that character: when editing `source/main.cpp`, stay in the flat, top-to-bottom style and do not refactor it into classes or helper functions.

## Two variants

The tutorial is built twice from one source directory, differing only in the windowing library, so that students can read both and the transition between them.

| target | source | windowing |
|---|---|---|
| `HowToVulkan` | `source/main.cpp` | SDL3 |
| `HowToVulkanGLFW` | `source/main-glfw.cpp` | GLFW |

`source/main.cpp` is the source of truth and tracks upstream. **`source/main-glfw.cpp` is generated and must never be hand-edited.** Change `main.cpp`, then regenerate:

```
python tools/make-glfw-variant.py            # rewrite main-glfw.cpp
python tools/make-glfw-variant.py --check    # fail if it is stale
```

The generator applies a fixed list of anchored replacements and refuses to run unless every anchor matches exactly once, so an upstream change that disturbs the windowing code fails loudly instead of silently producing a wrong file. Every anchor is ASCII C++ code, so re-wording comments is safe. `sdl-glfw-diff.md` is the hand-written companion table explaining each mapping.

To show the transition without switching branches:

```
git diff --no-index source/main.cpp source/main-glfw.cpp
```

## Build and run

CMake project rooted at `source/`; the build tree is at `build/` (repo root), generated with Visual Studio 17 2022. One configure builds both variants.

```
cmake -S source -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
```

Executables land in `build/bin/Debug/`. Neither starts unless `assets/` is reachable from the current working directory, so run from `source/`:

```
cd source && ../build/bin/Debug/HowToVulkanGLFW.exe
```

An optional first argument selects the physical device by index. CMake sets the Visual Studio debugger working directory to `source/` for both targets, so F5 works without extra setup.

There are no tests and no linter. Verification is visual: three textured Suzanne meshes, left-drag rotates the selected one, mouse wheel dollies the camera, `+`/`-` change the selection. Both variants should look identical at startup.

### Dependencies

SDL3, volk, VMA, glm and Slang all come from the LunarG Vulkan SDK via the `VULKAN_SDK` environment variable (install the optional components). GLFW is the exception: it does not ship with the SDK, so CMake fetches version 3.4 from source and links it statically, which means the GLFW variant needs no runtime DLL. KTX and tinyobjloader are vendored under `source/external/`, and both variants share `source/assets/` and `source/external/`. Slang versions after 2026.2 (including the one in SDK 1.4.357.0) have a shader-compilation bug; if shaders fail to compile, that is the first thing to check.

## Architecture of source/main.cpp

Everything below describes both variants, since they differ only in windowing. Globals at the top hold every Vulkan object; `main()` is a numbered sequence of 21 steps, and those numbers are the navigation aid. Roughly: instance and device (1-4), VMA (5), window and swapchain (6-7), depth image (8), vertex buffer from the `.obj` (8), per-frame uniform buffers (9), sync objects (10), command pool (11), KTX textures (12-13), descriptor set (14), Slang compilation (15-16), pipeline (18), render loop (19), swapchain recreation (20), teardown (21).

Deliberate API choices that shape the whole file:

- **Vulkan 1.3 core only.** `VK_KHR_swapchain` is the single device extension. Dynamic rendering and synchronization2 replace render passes and the old barrier API, so there is no `VkRenderPass` or `VkFramebuffer` anywhere.
- **volk loads entry points**, with `VK_NO_PROTOTYPES` set by CMake. `volkInitialize` runs before instance creation and `volkLoadInstance` right after.
- **Buffer device address instead of a uniform-buffer descriptor.** The per-frame shader data buffer's `VkDeviceAddress` is pushed as a push constant; the Slang vertex entry point takes it as a `uniform ShaderData*`. Slang turns that pointer parameter into the push constant, which is why the pipeline layout declares one.
- **Descriptor indexing for textures.** A single variable-count descriptor binding holds all three combined image samplers, indexed non-uniformly in the fragment shader. This is the only descriptor set.
- **Slang is compiled at runtime**, from `assets/shader.slang`, so editing the shader needs no rebuild. One module carries both the vertex and fragment entry points, hence a single `VkShaderModule` used for both pipeline stages.
- **VMA owns all allocations** for the depth image, textures, vertex buffer and uniform buffers.

### Frame synchronization

Two indices are in play and confusing them is the usual source of validation errors. `frameIndex` cycles `0..maxFramesInFlight-1` and selects CPU-side per-frame resources: command buffer, fence, uniform buffer, and the image-acquired semaphore. `imageIndex` comes from `vkAcquireNextImageKHR` and selects swapchain resources, including `renderCompleteSemaphores`, which is sized per swapchain image rather than per frame in flight so that present waits on the right semaphore.

Swapchain recreation on resize or `VK_ERROR_OUT_OF_DATE_KHR` reuses the stored `swapchainCI`, and must also rebuild the image views, the render-complete semaphores and the depth image.

## Keeping tutorial and code in sync

`tutorial/docs/index.md` quotes `source/main.cpp` extensively and is the published site (built with Zensical, configured in `tutorial/zensical.toml`). Upstream requires that any source change be reflected in the tutorial text. Apply the same rule here for changes meant to go upstream; fork-local experiments need not update the prose.

Upstream also rejects AI-generated pull requests and issues, so do not open any against `SaschaWillems/HowToVulkan`.

## Local working files

`source/main.cpp` used to be Windows-1250 with Czech comments. As of commit `8b43e15` those are translated and the file is plain ASCII with CRLF endings, so ordinary text tools work on it again. Keep it that way, and keep the generator's output CRLF too.

`source/main - kopie.cpp` and `source/main-orig.cpp` are untracked local scratch copies, the latter being the pristine upstream version. Do not commit them and do not treat them as build inputs.
