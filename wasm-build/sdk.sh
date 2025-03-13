#!/bin/bash
mkdir -p /tmp/sdk
if ${NO_SDK_CHECK:-false}
then
    exit 0
fi

# GLOBAL BASE probable bug here
# https://github.com/emscripten-core/emscripten/blob/ac676d5e437525d15df5fd46bc2c208ec6d376a3/tools/link.py#L1652-L1658


if python3 -V
then
    echo using installed python3
else
    echo wil use python for build as system python.
    ln -sf $SDKROOT/devices/$(arch)/usr/bin/python3 /usr/bin/python3
fi

if [ -f $SDKROOT/VERSION ]
then
    echo "Using tested sdk from $SDKROOT"
else
    if [ -d $SDKROOT/emsdk ]
    then
        echo "Using installed sdk from $SDKROOT"
    else
        echo "Installing sdk to $SDKROOT"
        SDK_ARCHIVE=${SDK_ARCHIVE:-python3.13-wasm-sdk-Ubuntu-22.04.tar.lz4}
        WASI_SDK_ARCHIVE=${WASI_SDK_ARCHIVE:-python3.13-wasi-sdk-Ubuntu-22.04.tar.lz4}
        if $CI
        then
            echo "if sdk fails here, check .yml files and https://github.com/pygame-web/python-wasm-sdk releases"
        fi
        echo https://github.com/pygame-web/python-wasm-sdk/releases/download/$SDK_VERSION/$SDK_ARCHIVE
        curl -sL --retry 5 https://github.com/pygame-web/python-wasm-sdk/releases/download/$SDK_VERSION/$SDK_ARCHIVE | tar xP --use-compress-program=lz4
        echo https://github.com/pygame-web/python-wasi-sdk/releases/download/$WASI_SDK_VERSION/$WASI_SDK_ARCHIVE
        curl -sL --retry 5 https://github.com/pygame-web/python-wasi-sdk/releases/download/$WASI_SDK_VERSION/$WASI_SDK_ARCHIVE | tar xP --use-compress-program=lz4
    fi

    pushd /tmp/sdk

#if false
#then
#    ${SDKROOT}/emsdk/upstream/bin/wasm-opt --version > ${SDKROOT}/wasm-opt.version
#    cat > ${SDKROOT}/emsdk/upstream/bin/wasm-opt <<END
##!/bin/bash
#if echo \$*|grep -q version$
#then
#	echo "$(cat ${SDKROOT}/wasm-opt.version)"
#else
#	# echo "\$@" >> /tmp/wasm.opt
#    exit 0
#fi
#END
#        chmod +x ${SDKROOT}/emsdk/upstream/bin/wasm-opt
#fi

    ALL="-m32 \
-D_FILE_OFFSET_BITS=64 \
-sSUPPORT_LONGJMP=emscripten \
-mno-bulk-memory \
-mnontrapping-fptoint \
-mno-reference-types \
-mno-sign-ext \
-mno-extended-const \
-mno-atomics \
-mno-tail-call \
-mno-fp16 \
-mno-multivalue \
-mno-relaxed-simd \
-mno-simd128 \
-mno-multimemory \
-mno-exception-handling"


        rm hello_em.*

        cat > /tmp/sdk/hello_em.c <<END
#include <stdio.h>
#include <assert.h>
#if defined(__EMSCRIPTEN__)
#include "emscripten.h"
#endif

#define IO ((char *)(1))

int main(int argc, char**arv){
#if defined(__EMSCRIPTEN__)
#   if defined(__PYDK__)
        printf("pydk" " %d.%d.%d\n",__EMSCRIPTEN_major__, __EMSCRIPTEN_minor__, __EMSCRIPTEN_tiny__);
#   else
        printf("emsdk" " %d.%d.%d\n",__EMSCRIPTEN_major__, __EMSCRIPTEN_minor__, __EMSCRIPTEN_tiny__);
#   endif
#else
    puts("native");
#endif
    {
        int first = 0;
        for (int i=0;i<256;i++)
            if ( IO[i] ) {
                if (!first)
                  first = i;
                printf("%c", IO[i] );
            } else {
                printf(".");
            }
        printf("\n\nBASE=%d\n", first);
        assert(first==31);
    }
    return 0;
}
END

        EMCC_TRACE=true DEBUG_PATTERN=* ${SDKROOT}/emsdk/upstream/emscripten/emcc -sASSERTIONS=0 -sENVIRONMENT=node,web -sGLOBAL_BASE=32B -o hello_em.html /tmp/sdk/hello_em.c
        $SDKROOT/emsdk/node/*.*.*64bit/bin/node hello_em.js
        $SDKROOT/emsdk/node/*.*.*64bit/bin/node hello_em.js |grep ^pydk > $SDKROOT/VERSION || exit 80
        rm hello_em.js hello_em.wasm

        python3 -E ${SDKROOT}/emsdk/upstream/emscripten/emcc.py $COPTS -sENVIRONMENT=node,web -sGLOBAL_BASE=32B $ALL -o hello_em.js /tmp/sdk/hello_em.c
        $SDKROOT/emsdk/node/*.*.*64bit/bin/node hello_em.js
        $SDKROOT/emsdk/node/*.*.*64bit/bin/node hello_em.js |grep ^emsdk >> $SDKROOT/VERSION || exit 84

        rm hello_em.*

    popd

fi

cat $SDKROOT/VERSION

