#!/bin/sh
# Regenerates the embedded SPIR-V headers used by tests/selftest.cpp.
set -e
cd "$(dirname "$0")/../tests/shaders"
for f in *.vert *.frag *.comp; do
    n=$(echo "$f" | tr . _)
    glslangValidator -V --vn "spv_$n" -o "$n.h" "$f" >/dev/null
done
