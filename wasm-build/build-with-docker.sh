#!/bin/bash

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="17.4_3.1.61.7bi"

[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig

source .buildconfig

cat .buildconfig

[ -f postgres-pglite/configure ] || ln -s . postgres-pglite

cd $(realpath ${WORKSPACE}/postgres-pglite)

export WORKSPACE=${GITHUB_WORKSPACE:-$(pwd)}

mkdir -p dist/pglite dist/extensions-emsdk

docker run \
  --rm \
  --env-file .buildconfig \
  --workdir=/workspace \
  -v ${WORKSPACE}/postgres-pglite:/workspace:rw \
  -v ${WORKSPACE}/postgres-pglite/dist:/tmp/sdk/dist:rw \
  $IMG_NAME:$IMG_TAG \
  bash -c "source ${SDKROOT}/wasm32-bi-emscripten-shell.sh && ./wasm-build.sh ${WHAT:-\"contrib extra\"}"

