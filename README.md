# How to Vulkan in 2026 - fork

Home of a minimalist tutorial on how to use the Vulkan graphics API in 2026. The idea is to get people started with rasterization in Vulkan using commonly supported features to make the API easier to use. This is a single page tutorial that can be done from top to bottom in a single day. I also tried to incorporate helpful tips, notes and warnings from my 10 years of experience working with Vulkan.

* 🔗 The tutorial is available at [www.howtovulkan.com](https://www.howtovulkan.com/)
* 💾 C++ source code can be found in the [in this folder](/source)
* 🎓 Tutorial text is located [in this folder](/tutorial/docs/index.md)
* 🌱 [Read this](CONTRIBUTING.md) if you want to contribute

# Extension of Sasha Willems original by Petr Felkel
1. `main.cpp` is now a more commented and slightly more formatted version by myself
2. `main-orig.cpp` is Sasha's original `main.cpp`
3. `main-glfw.cpp` is a claude generated replacement of SDL by GLFW  
4. [`sdl-glfw-diff.md`](../sdl-glfw-diff.md) is a table describing the changes from SDL to GLFW
   Use this table for orientation and `diff main.cpp main-glfw.cpp` for locating the specific lines.
   
- In case of any change in `main.cpp`:
	- generate the glfw version by 	`tools/make-glfw-variant.py`.
	- check the changes use `tools/make-glfw-variant.py --check`



