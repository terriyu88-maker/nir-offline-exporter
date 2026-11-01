# nir-offline-exporter

Offline Windows tool that takes SPIR-V, runs Mesa `spirv_to_nir()`, and dumps generic NIR (`nir_print_shader()`). It maps that in-memory `nir_shader` to a private IR whose reprint must `diff` empty against the NIR dump. JSON is a contrast dump of the same in-memory IR; there is no private binary format.

Pinned Mesa: **26.2.5** (vendored under `third_party/mesa`).

## Layout

| Path | Role |
|------|------|
| `third_party/mesa/` | Mesa 26.2.5 sources + MinGW cross file |
| `.monkeycode/docs/mesa-nir-architecture.md` | Mesa / compiler / NIR dump notes |
| `.monkeycode/specs/nir-offline-exporter/tasklist.md` | Staged plan |
| `shaders/min.frag` | Golden 1: ALU fragment |
| `shaders/tex.frag` | Golden 2: 2D texture sample |
| `shaders/ubo.vert` | Golden 3: UBO vertex |
| `shaders/ifloop.frag` | Golden 4: if / loop fragment |
| `shaders/ssbo.comp` | Golden 5: SSBO atomic compute |
| `out/min.frag.nir.txt` | Sample `nir_print_shader()` dump |
| `out/tex.frag.nir.txt` | Golden 2 Mesa dump |
| `out/ubo.vert.nir.txt` | Golden 3 Mesa dump |
| `out/ifloop.frag.nir.txt` | Golden 4 Mesa dump |
| `out/ssbo.comp.nir.txt` | Golden 5 Mesa dump |

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

## GLSL input

`.spv` is used as-is. `.vert` / `.frag` / `.comp` (and `.tesc` / `.tese` / `.geom` / `.mesh` / `.task`) are compiled with `glslangValidator -V` (or `glslang.exe`) then follow the same NIR path. Stage is inferred from the extension unless `--stage` is given.

```bash
spirv2nir.exe shaders/min.frag
spirv2nir.exe shaders/min.frag --emit ours
spirv2nir.exe shaders/min.frag --emit json -o out/min.frag.json
```

Find the glslang tool in this order: `GLSLANG_VALIDATOR`, next to `spirv2nir.exe`, then PATH. Without it, GLSL input fails with a clear error; pass `.spv` instead.

## Windows package

```bash
scripts/pack-windows.sh
```

Writes `dist/spirv2nir/spirv2nir.exe`, `libwinpthread-1.dll`, and `README.txt`. glslang is not bundled; copy `glslang.exe` from a Khronos Windows release next to the exe, or install the Vulkan SDK.

`objdump -p spirv2nir.exe` should list Windows DLLs only (`KERNEL32.dll`, `msvcrt.dll`, `libwinpthread-1.dll`, plus an API-set DLL).

## Local patches on Mesa 26.2.5

- `src/compiler/spirv/spirv2nir.c`: `fopen`/`fread` instead of POSIX `mmap`; always runs the L1 pass list then `nir_validate_shader()`; `--lower-io` is contrast-only; GLSL files call `glslangValidator -V`
- `meson.build`: `tools=nir` forces full NIR (`with_gfx_compute`), so `libnir` is not the stub

## Status

Stages 0–4 done. In-memory own IR reprints match `nir_print_shader()` (`diff` empty) on goldens 1–5. JSON contrast dump round-trips. GLSL files compile through glslang on Windows then follow the same path. Next optional: a clean Win10/11 machine without Mesa sources.
