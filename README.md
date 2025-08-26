# SDL

## Build

One-time configure step

```bash
cmake -S . -B build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
For debugging use the above with ``-DCMAKE_BUILD_TYPE=Debug``

Build and run

```bash
cmake --build build -j && ./build/sdlgpu_imgui_triangle
```
