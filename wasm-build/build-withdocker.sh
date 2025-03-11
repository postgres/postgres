#!/bin/bash

export WORKSPACE=${GITHUB_WORKSPACE:-$(pwd)}

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="latest"

[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig

source .buildconfig

if [[ -z "$SDKROOT" || -z "$PG_VERSION" ]]; then
  echo "Missing SDKROOT and PG_VERSION env vars."
  echo "Source them from .buildconfig"
  exit 1
fi


# release directory
mkdir -p ${WORKSPACE}/dist

# lib postgres "libpgcore.a" backend with no main/main.o tcop/postgres.o.
mkdir -p ${WORKSPACE}/dist/postgres-wasm

# full wasi postgres ( no extensions , only plpgsql and vector
mkdir -p ${WORKSPACE}/dist/postgres-wasi

# node/bun app with RAWFS support ( direct disk access from Node/Bun )
mkdir -p ${WORKSPACE}/dist/postgres-emsdk

# node/bun pglite with RAWFS support ( direct disk access from Node/Bun )
mkdir -p ${WORKSPACE}/dist/pglite-emsdk

# web+node pglite, smaller node fs mount on subfolders no direct disk access.
mkdir -p ${WORKSPACE}/dist/pglite-sandbox


# web only pglite - smallest - no node fs mount.
mkdir -p ${WORKSPACE}/dist/pglite-web



docker run \
  --rm \
  -e SDKROOT=$SDKROOT \
  -e PG_VERSION=${PG_VERSION} \
  -e PG_BRANCH=${PG_BRANCH} \
  -v /tmp/sdk:${WORKSPACE}/dist
  -v .:${WORKSPACE} \
  $IMG_NAME:$IMG_TAG \
  bash /workspace/wasm-build.sh ${WHAT:-"contrib extra"}



