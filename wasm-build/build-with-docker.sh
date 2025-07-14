#!/bin/bash

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME=${IMG_NAME:-"electricsql/pglite-builder"}

[ -f postgres-pglite/configure ] || ln -s . postgres-pglite

export WORKSPACE=${GITHUB_WORKSPACE:-$(pwd)}

# normally would default to /workspace but that may cause trouble with debug paths in some IDE
export DOCKER_WORKSPACE=${DOCKER_WORKSPACE:-$WORKSPACE}

cd $(realpath ${WORKSPACE}/postgres-pglite)

[ -f ${BUILD_CONFIG:-postgres-pglite}/.buildconfig ] && cp ${BUILD_CONFIG:-postgres-pglite}/.buildconfig .buildconfig
[ -f ./pglite/.buildconfig ] && cp ./pglite/.buildconfig .buildconfig


source .buildconfig

cat .buildconfig


if echo $IMG_NAME|grep -q debian
then
    IMG_NAME="debian"
    IMG_TAG="12"
    wget -q -Osdk.tar.lz4 \
     https://github.com/electric-sql/portable-sdk/releases/download/${SDK_VERSION}/python3.13-wasm-sdk-${IMG_NAME}${IMG_TAG}-$(arch).tar.lz4
else
    IMG_TAG="${PG_VERSION}_${SDK_VERSION}"
fi


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
  -e WASI=${WASI:-false} \
  --workdir=${DOCKER_WORKSPACE} \
  -v ${WORKSPACE}/postgres-pglite:${DOCKER_WORKSPACE}:rw \
  -v ${WORKSPACE}/postgres-pglite/dist:/tmp/sdk/dist:rw \
  $IMG_NAME:$IMG_TAG \
  bash --noprofile --rcfile ./docker_rc.sh -ci "( ./wasm-build.sh ${WHAT:-\"contrib extra\"} $PROMPT"
