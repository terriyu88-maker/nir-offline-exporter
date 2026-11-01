#!/bin/sh
# Assemble a Windows zip next to the MinGW spirv2nir.exe.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exe="$root/third_party/mesa/build-mingw/src/compiler/spirv/spirv2nir.exe"
dll="$root/third_party/mesa/build-mingw/src/compiler/spirv/libwinpthread-1.dll"
out="$root/dist/spirv2nir"

if [ ! -f "$exe" ]; then
   echo "missing $exe" >&2
   echo "build first: ninja -C third_party/mesa/build-mingw src/compiler/spirv/spirv2nir.exe" >&2
   exit 1
fi

mkdir -p "$out"
cp "$exe" "$out/spirv2nir.exe"

if [ ! -f "$dll" ]; then
   dll=/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll
fi
if [ ! -f "$dll" ]; then
   echo "missing libwinpthread-1.dll" >&2
   exit 1
fi
cp "$dll" "$out/libwinpthread-1.dll"

cat > "$out/README.txt" <<'EOF'
spirv2nir.exe  --  Mesa 26.2.5 generic NIR dump (Windows x86-64)

Usage:
  spirv2nir.exe shader.spv
  spirv2nir.exe shader.frag
  spirv2nir.exe shader.spv --emit ours
  spirv2nir.exe shader.spv --emit json -o shader.json
  spirv2nir.exe --from-json shader.json

Input:
  .spv                         SPIR-V, used as-is
  .vert/.frag/.comp/...        GLSL, compiled with glslangValidator -V

GLSL needs a Windows glslang binary:
  1. Set GLSLANG_VALIDATOR to glslangValidator.exe or glslang.exe, or
  2. Copy that exe next to spirv2nir.exe, or
  3. Install the Vulkan SDK so it is on PATH.

This zip does not ship glslang. Download a Windows release from
https://github.com/KhronosGroup/glslang/releases
(for example glslang-main-windows-x86_64-release.zip) and copy bin/glslang.exe
next to spirv2nir.exe.

Keep libwinpthread-1.dll next to spirv2nir.exe.
EOF

echo "packed $out"
ls -l "$out"
