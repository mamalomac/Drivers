#!/bin/sh
# Cross-compiles the driver on Linux (needs g++-mingw-w64-x86-64, cmake, ninja)
# and assembles the ready-to-install dist/ folder.
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build
mkdir -p dist
cp build/vkon11.dll build/tests/selftest.exe dist/
x86_64-w64-mingw32-strip dist/vkon11.dll dist/selftest.exe
cp install/vkon11_icd.json install/install.bat install/uninstall.bat install/selftest.bat dist/
# Windows batch files want CRLF line endings.
sed -i 's/\r$//; s/$/\r/' dist/*.bat
ls -l dist
