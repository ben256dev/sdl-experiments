# SDL

## Build

One-time configure step

```bash
cmake -S . -B build
```

For debugging use the above with ``-DCMAKE_BUILD_TYPE=Debug``

Build and run

```bash
cmake --build build -j && ./build/sdlgpu_imgui_triangle
```
