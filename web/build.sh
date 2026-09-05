#!/bin/sh
# Build the browser module from the systemless fork checked out beside this
# repository, and put it where www/index.html loads it.
#
# Two toolchain facts, both learned the hard way on 4 September 2026:
# - rust-lld is dynamically linked against the toolchain's own libLLVM.dylib
#   and is spawned without that directory on its loader path, so the link
#   step aborts with "Library not loaded: @rpath/libLLVM.dylib" unless
#   DYLD_FALLBACK_LIBRARY_PATH names it. `cargo check` never links, which is
#   why tools/wasm_check.sh never saw this.
# - Homebrew's rust has no wasm32 standard library and wins on PATH, so the
#   rustup toolchain is named explicitly, as tools/wasm_check.sh does.
set -e
TC="${RUSTUP_TOOLCHAIN_DIR:-$HOME/.rustup/toolchains/stable-aarch64-apple-darwin}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${CARGO_TARGET_DIR:-$HERE/target}"
cd "$HERE/cythera-web"
DYLD_FALLBACK_LIBRARY_PATH="$TC/lib" PATH="$TC/bin:$PATH" RUSTC="$TC/bin/rustc" \
  CARGO_TARGET_DIR="$TARGET" "$TC/bin/cargo" build --release --target wasm32-unknown-unknown "$@"
cp "$TARGET/wasm32-unknown-unknown/release/cythera_web.wasm" "$HERE/www/"
ls -la "$HERE/www/cythera_web.wasm"
