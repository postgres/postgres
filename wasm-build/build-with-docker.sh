#!/bin/bash

# export WORKSPACE=${GITHUB_WORKSPACE:-/workspace}

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="17.4_3.1.61.6bi"

[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig

source .buildconfig

if [[ -z "$SDKROOT" || -z "$PG_VERSION" ]]; then
  echo "Missing SDKROOT and PG_VERSION env vars."
  echo "Source them from .buildconfig"
  exit 1
fi

docker run \
  --rm \
  -e SDKROOT=$SDKROOT \
  -e PG_VERSION=${PG_VERSION} \
  -e PG_BRANCH=${PG_BRANCH} \
  -v .:/src:ro \
  -v ./dist:/tmp/dist:rw \
  $IMG_NAME:$IMG_TAG \
  bash -c "cp -r /src/. /workspace && source /tmp/sdk/wasm32-bi-emscripten-shell.sh && /workspace/wasm-build.sh ${WHAT:-\"contrib extra\"}"



