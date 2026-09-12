# SDL3 to GLFW port

Mapping of every difference between the two variants of this tutorial:

| target | source | windowing |
|---|---|---|
| `HowToVulkan` | `source/main.cpp` | SDL3 |
| `HowToVulkanGLFW` | `source/main-glfw.cpp` | GLFW 3.4 |

No Vulkan call differs between them; only windowing, input, timing, the surface and
the build system do. The table below is the summary, and the exact diff is:

```
git diff --no-index source/main.cpp source/main-glfw.cpp
```

`main-glfw.cpp` is generated from `main.cpp` by `tools/make-glfw-variant.py`, so the
two cannot drift apart. Edit `main.cpp` and re-run the generator; never edit the
GLFW file by hand.

| SDL | GLFW | `       comment        `|
|--|--|---|
| `#include <SDL3/SDL.h>`<br>`#include <SDL3/SDL_vulkan.h>` | `#define GLFW_INCLUDE_NONE`<br>`#include <GLFW/glfw3.h>` | `vulkan.h` is already included above, so glfw3.h declares its Vulkan entry points; `GLFW_INCLUDE_NONE` keeps the GL headers out |
| *(implicit)* | `#include <cassert>` | SDL pulled in `assert.h` transitively, GLFW does not, and `assert` is used three times |
| `chk(SDL_Init(SDL_INIT_VIDEO))` | `chk(glfwInit())` | both return something the existing `chk(bool)` accepts |
| `chk(SDL_Vulkan_LoadLibrary(NULL))` | *(dropped)* | GLFW loads the Vulkan loader itself, on demand |
| `SDL_Vulkan_GetInstanceExtensions(&count)` | `glfwGetRequiredInstanceExtensions(&count)` | same shape, result still assigned to `char const* const*` |
| `SDL_Vulkan_GetPresentationSupport(instance, dev, qf)` | `glfwGetPhysicalDevicePresentationSupport(instance, dev, qf)` | identical argument order |
| `SDL_Window* window` | `GLFWwindow* window` | |
| `SDL_CreateWindow(title, 1280u, 720u, SDL_WINDOW_VULKAN \| SDL_WINDOW_RESIZABLE)` | `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)`<br>`glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE)`<br>`glfwCreateWindow(1280, 720, title, nullptr, nullptr)` | flags become hints set before creation; size precedes title; `GLFW_NO_API` is what suppresses the OpenGL context |
| `SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)` | `glfwCreateWindowSurface(instance, window, nullptr, &surface)` | instance and window swap places, both return `VkResult` |
| `SDL_GetWindowSize(window, &windowSize.x, &windowSize.y)` | `glfwGetFramebufferSize(window, &windowSize.x, &windowSize.y)` | framebuffer size, not window size, is what the swapchain extent needs; returns void so `chk` drops |
| `uint64_t lastTime{ SDL_GetTicks() }` | `double lastTime{ glfwGetTime() }` | GLFW counts seconds, so the `/ 1000.0f` disappears |
| `for (SDL_Event event; SDL_PollEvent(&event);) { ... }` | `glfwPollEvents()` plus four callbacks registered after window creation | GLFW has no queue to drain; this is the only structural change in the file |
| `event.type == SDL_EVENT_QUIT` | `glfwWindowShouldClose(window)` | checked right after polling instead of inside the loop |
| `SDL_EVENT_MOUSE_MOTION`, `event.motion.xrel/yrel` | `glfwSetCursorPosCallback` plus a saved `lastMousePos` | GLFW reports absolute positions only, so the delta is derived |
| `event.button.button == SDL_BUTTON_LEFT` | `glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS` | the SDL form read the button field of a motion event, which aliased the button mask |
| `SDL_EVENT_MOUSE_WHEEL`, `event.wheel.y` | `glfwSetScrollCallback`, `yoffset` | |
| *(per-event `* elapsedTime`)* | `mouseDelta` and `scrollDelta` accumulated, applied once per frame | numerically identical, since elapsed time is constant within a frame |
| `SDL_EVENT_KEY_DOWN` | `glfwSetKeyCallback`, filtered to `GLFW_PRESS` | repeats ignored, matching SDL's behaviour here |
| `SDLK_PLUS`, `SDLK_KP_PLUS` | `GLFW_KEY_EQUAL`, `GLFW_KEY_KP_ADD` | GLFW names keys by physical position, so the top-row key is layout-dependent; the numpad key is not |
| `SDLK_MINUS`, `SDLK_KP_MINUS` | `GLFW_KEY_MINUS`, `GLFW_KEY_KP_SUBTRACT` | same caveat |
| `SDL_EVENT_WINDOW_RESIZED` | `glfwSetFramebufferSizeCallback` | both just set `updateSwapchain` |
| `SDL_DestroyWindow`<br>`SDL_QuitSubSystem(SDL_INIT_VIDEO)`<br>`SDL_Quit()` | `glfwDestroyWindow`<br>`glfwTerminate()` | one call fewer, there is no subsystem to quit separately |


## Build file

`source/CMakeLists.txt` builds both variants from one configure. Below are the lines
that differ, compared against the original single-variant file.

| SDL | GLFW | `       comment        `|
|--|--|---|
| `find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)` | `FetchContent_Declare(GLFW`<br>`  GIT_REPOSITORY https://github.com/glfw/glfw`<br>`  GIT_TAG 3.4 ...)`<br>`FetchContent_MakeAvailable(GLFW)` | SDL3 ships with the Vulkan SDK and only has to be located; GLFW does not, so it is fetched and built from source, the same pattern the file already used for GLM |
| *(nothing needed)* | `set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)`<br>`set(GLFW_BUILD_TESTS OFF ...)`<br>`set(GLFW_BUILD_EXAMPLES OFF ...)`<br>`set(GLFW_INSTALL OFF ...)` | must come before `FetchContent_MakeAvailable`, otherwise GLFW builds its docs, tests and examples too |
| `add_executable(${NAME} main.cpp assets/shader.slang)` | `add_executable(${NAME}GLFW main-glfw.cpp assets/shader.slang)` | one target per variant; the shader is listed only so it shows up in the IDE project |
| the `target_compile_definitions`, `set_target_properties` and `target_compile_features` calls, written once | the same calls wrapped in `foreach(TUTORIAL_TARGET ${NAME} ${NAME}GLFW)` | these settings are identical for both variants, so they are written once instead of duplicated |
| `target_link_libraries(${NAME} PRIVATE ${SDL_LIBRARY} ktx ${Slang_LIBRARY})` | `target_link_libraries(${TUTORIAL_TARGET} PRIVATE ktx ${Slang_LIBRARY})` inside the loop, then<br>`target_link_libraries(${NAME} PRIVATE ${SDL_LIBRARY})`<br>`target_link_libraries(${NAME}GLFW PRIVATE glfw)` | the shared libraries move into the loop, so the two adjacent lines left over are the whole build difference between the variants |
| executable needs `SDL3.dll` on `PATH` or beside it | nothing to copy | GLFW builds as a static library by default, so the GLFW executable has no runtime dependency |


## Verification

Configured and built with Visual Studio 17 2022, then run with the Khronos validation
layer force-enabled through `VK_LOADER_LAYERS_ENABLE`. The loader log confirms the layer
loaded, and it reported nothing across every run.

Checked by screenshot that the scene renders, that the selection key cycles the highlight
through all three models and wraps (1 -> 2 -> 0 -> 1 -> 2 over four presses), that the
wheel dollies the camera, that left-drag rotates the selected model, that resizing
rebuilds the swapchain and keeps rendering, and that closing the window exits cleanly.
