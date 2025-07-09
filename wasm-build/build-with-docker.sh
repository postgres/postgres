#!/bin/bash

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="17.4_3.1.61.7bi"

[ -f postgres-pglite/configure ] || ln -s . postgres-pglite

export WORKSPACE=${GITHUB_WORKSPACE:-$(pwd)}

# normally would default to /workspace but that may cause trouble with debug paths in some IDE
export DOCKER_WORKSPACE=${DOCKER_WORKSPACE:-$WORKSPACE}

cd $(realpath ${WORKSPACE}/postgres-pglite)

[ -f ${BUILD_CONFIG:-postgres-pglite}/.buildconfig ] && cp ${BUILD_CONFIG:-postgres-pglite}/.buildconfig .buildconfig
[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig


source .buildconfig

cat .buildconfig


mkdir -p dist/pglite dist/extensions-emsdk

if echo -n $@|grep -q it$
then
    PROMPT="&& bash ) || bash"
else
    PROMPT=")"
fi

docker run $@ \
  --rm \
  --env-file .buildconfig \
  -e DEBUG=${DEBUG:-false} \
  --workdir=${DOCKER_WORKSPACE} \
  -v ${WORKSPACE}/postgres-pglite:${DOCKER_WORKSPACE}:rw \
  -v ${WORKSPACE}/postgres-pglite/dist:/tmp/sdk/dist:rw \
  $IMG_NAME:$IMG_TAG \
  bash --noprofile --rcfile ${SDKROOT}/wasm32-bi-emscripten-shell.sh -ci "( ./wasm-build.sh ${WHAT:-\"contrib extra\"} $PROMPT"
