# nir-offline-exporter

Offline Windows tool that takes SPIR-V, runs Mesa `spirv_to_nir()`, and dumps generic NIR (`nir_print_shader()`). Later stages map that in-memory `nir_shader` to a private IR whose reprint must `diff` empty against the NIR dump.

Pinned Mesa: **26.2.5** (vendored under `third_party/mesa`).

## Layout

| Path | Role |
|------|------|
| `third_party/mesa/` | Mesa 26.2.5 sources + MinGW cross file |
| `.monkeycode/docs/mesa-nir-architecture.md` | Mesa / compiler / NIR dump notes |
| `.monkeycode/specs/nir-offline-exporter/tasklist.md` | Staged plan |
| `shaders/min.frag` | Golden 1: ALU fragment |
| `shaders/tex.frag` | Golden 2: 2D texture sample |
| `out/min.frag.nir.txt` | Sample `nir_print_shader()` dump |
| `out/tex.frag.nir.txt` | Golden 2 Mesa dump |

## Cross-build (Linux → Windows PE)

Needs: meson, ninja, python3-mako, pyyaml, bison, flex, `x86_64-w64-mingw32-gcc/g++`, wine.

```bash
cd third_party/mesa
meson setup build-mingw \
  --cross-file mingw-w64.ini \
  --backend=ninja \
  -Dbuildtype=release \
  -Dplatforms=windows \
  -Dgallium-drivers= \
  -Dvulkan-drivers= \
  -Dllvm=disabled \
  -Dopengl=false \
  -Degl=disabled \
  -Dgbm=disabled \
  -Dglx=disabled \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dtools=nir \
  -Dzlib=disabled \
  -Dzstd=disabled \
  -Dvalgrind=disabled \
  -Dlibunwind=disabled \
  -Dlmsensors=disabled \
  -Dmicrosoft-clc=disabled \
  -Dspirv-tools=disabled \
  -Dxmlconfig=disabled \
  -Dexpat=disabled \
  -Dshader-cache=disabled \
  -Dvideo-codecs= \
  -Dgallium-va=disabled \
  -Dgallium-mediafoundation=disabled \
  -Dbuild-tests=false
ninja -C build-mingw src/compiler/spirv/spirv2nir.exe -j2
```

Smoke with Wine (`WINEPREFIX` plus `libwinpthread-1.dll` next to the exe):

```bash
cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll \
  third_party/mesa/build-mingw/src/compiler/spirv/
glslangValidator -V -S frag -o shaders/min.frag.spv shaders/min.frag
WINEPREFIX=/tmp/wine-spirv2nir PATH="/usr/lib/wine:$PATH" \
  wine64 third_party/mesa/build-mingw/src/compiler/spirv/spirv2nir.exe shaders/min.frag.spv
```

`--lower-io` is contrast-only: after L1 it runs `nir_lower_io` on shader in/out so the dump can be diffed against the default deref IO.

## Local patches on Mesa 26.2.5

- `src/compiler/spirv/spirv2nir.c`: `fopen`/`fread` instead of POSIX `mmap`; always runs the L1 pass list then `nir_validate_shader()`; `--lower-io` is contrast-only
- `meson.build`: `tools=nir` forces full NIR (`with_gfx_compute`), so `libnir` is not the stub

## Status

Stage 2 golden 1–2 done: `--emit ours` reprints the same text as `nir_print_shader()` on `shaders/min.frag.spv` and `shaders/tex.frag.spv` (`diff` empty). Covered: `deref_var`, `@load_deref`, `load_const`, `fmul`, `@store_deref`, `(float32)tex` with texture/sampler deref. Phi / Undef remain placeholders. Next: golden 3–5 (UBO / if-loop / SSBO), then stage 3 JSON.
