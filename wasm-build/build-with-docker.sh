#!/bin/bash

# export WORKSPACE=${GITHUB_WORKSPACE:-/workspace}

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="17.4_3.1.61.7bi"

[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig

source .buildconfig

# if [[ -z "$SDKROOT" || -z "$PG_VERSION" ]]; then
#   echo "Missing SDKROOT and PG_VERSION env vars."
#   echo "Source them from .buildconfig"
#   exit 1
# fi

cat .buildconfig

docker run \
  --rm \
  --env-file .buildconfig \
  -v .:/workspace:rw \
  -v ./dist:/tmp/sdk/dist:rw \
  $IMG_NAME:$IMG_TAG \
  bash -c "source ${SDKROOT}/wasm32-bi-emscripten-shell.sh && ./wasm-build.sh ${WHAT:-\"contrib extra\"}"
