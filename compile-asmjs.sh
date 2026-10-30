#!/bin/bash

# Builds PicoDrive as asm.js (pure JavaScript, no WebAssembly) for the browser, using
# Makefile.web. The Emscripten SDK has to be set up already (emsdk_env), this script
# doesn't install or update it. The output is build-web/picodrive.js (plus index.html).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="$(sysctl -n hw.ncpu 2> /dev/null || nproc)"
OUTPUT="build-web/picodrive.js"

if ! command -v emcc > /dev/null; then
    echo "emcc not found, set up the Emscripten SDK environment first (emsdk_env)"
    exit 1
fi

echo "Using $(emcc --version | head -n1)"

cd "${SCRIPT_DIR}"

# A clean build, so the objects and the output always come from these settings.
make -f Makefile.web clean
make -f Makefile.web -j"${JOBS}"

# Makefile.web already builds asm.js (WASM=0) with -O2, but it doesn't set
# ENVIRONMENT=web, which leaves out the code that only runs in Node. Its LDFLAGS can't be
# extended from outside, so the build is linked again with the flag in EMCC_CFLAGS, which
# emcc adds to its own command. Only the link runs here, since the objects are up to date.
rm -f "${OUTPUT}"
EMCC_CFLAGS="-sENVIRONMENT=web" make -f Makefile.web

# The output is meant for browsers, so it must not load anything with require.
if grep -qE '\brequire\(' "${OUTPUT}"; then
    echo "Error: ${OUTPUT} still has a require"
    exit 1
fi

echo "Done: ${SCRIPT_DIR}/${OUTPUT}"
