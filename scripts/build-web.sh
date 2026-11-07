#!/bin/sh
# Builds the WebAssembly module into web/, ready to serve: python3 -m http.server -d web
# Needs clang with the wasm32 target: wasi-sdk (WASI_SDK=/path/to/wasi-sdk) or a system clang + wasm-ld.
set -eu
cd "$(dirname "$0")/.."
if [ -n "${WASI_SDK:-}" ]; then CLANG="$WASI_SDK/bin/clang"; else CLANG="${CLANG:-clang}"; fi
"$CLANG" --target=wasm32 -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -nostdlib -mbulk-memory \
    -Wl,--no-entry -Wl,--strip-all -o web/mattone.wasm \
    src/cpu.c src/bus.c src/ppu.c src/cart.c src/wasm.c
echo "web/mattone.wasm: $(wc -c < web/mattone.wasm) bytes"
