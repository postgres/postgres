#!/bin/bash

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="17.4_3.1.61.7bi"

[ -f postgres-pglite/configure ] || ln -s . postgres-pglite

export WORKSPACE=${GITHUB_WORKSPACE:-$(pwd)}

cd $(realpath ${WORKSPACE}/postgres-pglite)

[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig

source .buildconfig

cat .buildconfig


mkdir -p dist/pglite dist/extensions-emsdk

if echo -n $@|grep -q it$
then
    PROMPT="|| bash"
fi

docker run $@ \
  --rm \
  --env-file .buildconfig \
  --workdir=/workspace \
  -v ${WORKSPACE}/postgres-pglite:/workspace:rw \
  -v ${WORKSPACE}/postgres-pglite/dist:/tmp/sdk/dist:rw \
  $IMG_NAME:$IMG_TAG \
  bash --noprofile --rcfile ${SDKROOT}/wasm32-bi-emscripten-shell.sh -ci "./wasm-build.sh ${WHAT:-\"contrib extra\"} $PROMPT"

