#!/bin/bash
pushd ../contrib/pgcrypto
# these flags are used in pgxs.mk (postgresql extension makefile) and passed to the build process of that extension
emmake make LDFLAGS_SL="-sWASM_BIGINT -sSIDE_MODULE=1 -fexceptions -Wl,--whole-archive -lssl -lcrypto -Wl,--no-whole-archive" CFLAGS_SL="$PGLITE_CFLAGS -fexceptions -sWASM_BIGINT" -j
# emmake make PORTNAME=emscripten dist
popd