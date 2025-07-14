#!/bin/bash

WORKSPACE=$(pwd)

echo "

SDK prepare

"

pushd /
    # this is minimum required to **use** sdk on docker/debian 12, not build it
    apt-get update && apt-get --yes install git wget curl lz4 xz-utils bison flex pkg-config autoconf make

    if [ -f $WORKSPACE/sdk.tar.lz4 ]
    then
        tar xf $WORKSPACE/sdk.tar.lz4 --use-compress-program=lz4
    fi

    if [ -d $SDKROOT/wasisdk/upstream ]
    then
	    echo "wasi sdk common support is already installed"
    else
	    tar xf ${WORKSPACE}/prebuilt/wasi-sdk-25.tar.xz
    fi

    if [ -d $SDKROOT/wasisdk/upstream/lib ]
    then
	    echo "wasi sdk $(arch) support is already installed"
    else
	    pushd $SDKROOT/wasisdk
	        if arch|grep -q aarch64
	        then
		        wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-arm64-linux.tar.gz -O/tmp/sdk.tar.gz
	        else
		        wget https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-x86_64-linux.tar.gz -O/tmp/sdk.tar.gz
	        fi
	        tar xfz /tmp/sdk.tar.gz && rm /tmp/sdk.tar.gz
	        mv wasi-sdk-25.0-*/{bin,lib} upstream/
	    popd
    fi

popd

echo "

Setting up SDK shell

"

${SDKROOT}/wasm32-bi-emscripten-shell.sh
