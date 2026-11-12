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
| `shaders/phi.frag.spvasm` | Golden 6: fragment with `OpPhi` (needs `--optimize`); golden 15 is the same SPV at default L1 |
| `shaders/discard.frag` | Golden 7: `discard` + `demote` fragment |
| `shaders/push.frag` | Golden 8: push constant fragment |
| `shaders/texvar.frag` | Golden 9.1: cube / 2D array / shadow |
| `shaders/texop.frag` | Golden 9.2: `txl` / `txd` / `txf` / `tg4` |
| `shaders/image.comp` | Golden 10: `image2D` load/store |
| `shaders/shared.comp` | Golden 11: shared + barrier |
| `shaders/sysval.frag` | Golden 12.1: FS FragCoord / FrontFacing / FragDepth |
| `shaders/sysval.vert` | Golden 12.2: VS VertexIndex / InstanceIndex |
| `shaders/interp.frag` | Golden 13: `flat` / `noperspective` |
| `shaders/geom.geom` | Golden 14.1: geometry `EmitVertex` / `EndPrimitive` |
| `shaders/tess.tesc` | Golden 14.2: tess control `gl_TessLevel*` patch |
| `shaders/tess.tese` | Golden 14.2: tess eval `gl_TessCoord` |
| `shaders/undef.frag.spvasm` | Golden 16: `OpUndef` → `= undefined` |
| `shaders/swcont.frag` | Golden 17: `switch` + loop `continue` |
| `out/min.frag.nir.txt` | Sample `nir_print_shader()` dump |
| `out/tex.frag.nir.txt` | Golden 2 Mesa dump |
| `out/ubo.vert.nir.txt` | Golden 3 Mesa dump |
| `out/ifloop.frag.nir.txt` | Golden 4 Mesa dump |
| `out/ssbo.comp.nir.txt` | Golden 5 Mesa dump |
| `out/phi.frag.nir.txt` | Golden 6 Mesa dump (`--optimize`) |
| `out/phi.frag.l1.nir.txt` | Golden 15 Mesa dump (default L1: `function_temp` store/load) |
| `out/discard.frag.nir.txt` | Golden 7 Mesa dump |
| `out/push.frag.nir.txt` | Golden 8 Mesa dump |
| `out/texvar.frag.nir.txt` | Golden 9.1 Mesa dump |
| `out/texop.frag.nir.txt` | Golden 9.2 Mesa dump |
| `out/image.comp.nir.txt` | Golden 10 Mesa dump |
| `out/shared.comp.nir.txt` | Golden 11 Mesa dump |
| `out/sysval.frag.nir.txt` | Golden 12.1 Mesa dump |
| `out/sysval.vert.nir.txt` | Golden 12.2 Mesa dump |
| `out/interp.frag.nir.txt` | Golden 13 Mesa dump |
| `out/geom.geom.nir.txt` | Golden 14.1 Mesa dump |
| `out/tess.tesc.nir.txt` | Golden 14.2 TCS Mesa dump |
| `out/tess.tese.nir.txt` | Golden 14.2 TES Mesa dump |
| `out/undef.frag.nir.txt` | Golden 16 Mesa dump (`= undefined`) |
| `out/swcont.frag.nir.txt` | Golden 17 Mesa dump (`switch` → ifs, `loop_continue`) |

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

Stages 0–4 done. In-memory own IR reprints match `nir_print_shader()` (`diff` empty) on goldens 1–17. Golden 6 (`shaders/phi.frag.spv` + `--optimize`) keeps a NIR `phi`; golden 15 is the same SPV at default L1 (`function_temp` store/load, no `phi`). Golden 7 maps GLSL `discard` to `@terminate` and `demote` to `@demote`. Golden 8 is `layout(push_constant)` (`decl_var push_const`). Golden 9 covers cube/array/shadow and `txl`/`txd`/`txf`/`tg4` (`textureOffset` stays `tex` + offset). Golden 10 is `image2D` `@image_deref_load` / `@image_deref_store` with `r32g32b32a32_float`. Golden 11 is `shared` + `@barrier` (`execution_scope=WORKGROUP`, `mem_semantics=ACQ|REL`, `mem_modes=shared`). Golden 12 is FS `gl_FragCoord` / `gl_FrontFacing` / `gl_FragDepth` (`depth_layout: 1`) and VS `gl_VertexIndex` / `gl_InstanceIndex`. Golden 13 is `flat` / `noperspective` (`INTERP_MODE_FLAT` / `INTERP_MODE_NOPERSPECTIVE`). Golden 14 is GS (`TRIANGLE_STRIP` / `TRIANGLES`) and TCS/TES (`decl_var patch ... compact`, `gl_TessCoord`). Golden 16 is `OpUndef` (`32 %0 = undefined` then `fadd`). Golden 17 is `switch` + loop `continue` (L1: if-chain + `function_temp loop_continue`; no `continue` jump). JSON contrast dump round-trips. GLSL files compile through glslang on Windows then follow the same path. Next optional: a clean Win10/11 machine without Mesa sources.
