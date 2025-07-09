#!/bin/bash

WASI=${WASI:-false}
SDKROOT=${SDKROOT:-/tmp/sdk}
mkdir -p ${SDKROOT}

if [ -f /alpine ]
then
    cp -f /usr/bin/node ${SDKROOT}/emsdk/node/*.*.*/bin/
fi

# always install wasmtime because wasm-objdump needs it.
if [ -f ${SDKROOT}/devices/$(arch)/usr/bin/wasmtime ]
then
    echo "keeping installed wasmtime and wasi binaries"
else
# TODO: window only has a zip archive, better use wasmtime-py instead.
    export PLATFORM=$($SYS_PYTHON -E -c "print(__import__('sys').platform)")
    echo "

    ! wasmtime required ! installing from github release wasmtime-v33.0.0-$(arch)-${PLATFORM}.tar.xz

    "


    wget https://github.com/bytecodealliance/wasmtime/releases/download/v33.0.0/wasmtime-v33.0.0-$(arch)-${PLATFORM}.tar.xz \
     -O-|xzcat|tar xfv -
    mv -vf $(find wasmtime*|grep /wasmtime$) ${SDKROOT}/devices/$(arch)/usr/bin
fi

if $WASI
then
    if [ -f ${WASI_SYSROOT}/extra ]
    then
        echo -n
    else
        echo "

   * add wasi third parties to ${WASI_SYSROOT}

        "
        mkdir -p ${WASI_SYSROOT}
        pushd ${WASI_SYSROOT}
            VMLABS="https://github.com/vmware-labs/webassembly-language-runtimes/releases/download"
            wget -q "${VMLABS}/libs%2Flibpng%2F1.6.39%2B20230629-ccb4cb0/libpng-1.6.39-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Fzlib%2F1.2.13%2B20230623-2993864/libz-1.2.13-wasi-sdk-20.0.tar.gz"  -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Fsqlite%2F3.42.0%2B20230623-2993864/libsqlite-3.42.0-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Flibxml2%2F2.11.4%2B20230623-2993864/libxml2-2.11.4-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Fbzip2%2F1.0.8%2B20230623-2993864/libbzip2-1.0.8-wasi-sdk-20.0.tar.gz"  -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Flibuuid%2F1.0.3%2B20230623-2993864/libuuid-1.0.3-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            cat > ./include/wasm32-wasi/__struct_sockaddr_un.h <<END
#ifndef __wasilibc___struct_sockaddr_un_h
#define __wasilibc___struct_sockaddr_un_h

#include <__typedef_sa_family_t.h>

struct sockaddr_un {
    __attribute__((aligned(__BIGGEST_ALIGNMENT__))) sa_family_t sun_family;
	char sun_path[108];
};

#endif
END

            for level in p1 p2
            do
                cp -Rn lib/wasm32-wasi/* lib/wasm32-wasi${level}/
                cp include/wasm32-wasi/__struct_sockaddr_un.h include/wasm32-wasi${level}/__struct_sockaddr_un.h
                cp include/wasm32-wasi/sys/shm.h include/wasm32-wasi${level}/sys/shm.h
                cp include/wasm32-wasi/bits/shm.h include/wasm32-wasi${level}/bits/shm.h
                sed -i 's|extern FILE \*const stdin;|extern FILE \* stdin;|g'  include/wasm32-wasi${level}/stdio.h
                sed -i 's|extern FILE \*const stdout;|extern FILE \* stdout;|g'  include/wasm32-wasi${level}/stdio.h
                sed -i 's|extern FILE \*const stderr;|extern FILE \* stderr;|g'  include/wasm32-wasi${level}/stdio.h

                sed -i 's|int getrusage|//int getrusage|g' include/wasm32-wasi${level}/__header_sys_resource.h
            done
        popd
        touch ${WASI_SYSROOT}/extra
    fi
    exit 0
fi

# this one is critical for release mode
if grep -q __emscripten_tempret_get ${SDKROOT}/emsdk/upstream/emscripten/src/library_dylink.js
then
    echo -n
else
    pushd ${SDKROOT}/emsdk
    patch -p1 <<END
--- emsdk/upstream/emscripten/src/library_dylink.js
+++ emsdk.fix/upstream/emscripten/src/library_dylink.js
@@ -724,6 +724,8 @@
             stubs[prop] = (...args) => {
               resolved ||= resolveSymbol(prop);
               if (!resolved) {
+                if (prop==='getTempRet0')
+                    return __emscripten_tempret_get(...args);
                 throw new Error(\`Dynamic linking error: cannot resolve symbol \${prop}\`);
               }
               return resolved(...args);
END
    # this one for debug mode and changing  -Wl,--global-base= with -sGLOBAL_BASE
    patch -p1 <<END
--- emsdk/upstream/emscripten/tools/link.py	2025-06-23 08:45:26.554013381 +0200
+++ emsdk.fix/upstream/emscripten/tools/link.py	2025-06-23 08:45:31.445921560 +0200
@@ -1662,7 +1662,7 @@
     # use a smaller LEB encoding).
     # However, for debugability is better to have the stack come first
     # (because stack overflows will trap rather than corrupting data).
-    settings.STACK_FIRST = True
+    settings.STACK_FIRST = False

   if state.has_link_flag('--stack-first'):
     settings.STACK_FIRST = True
END

    popd

fi


if ${NO_SDK_CHECK:-false}
then
    exit 0
fi

# GLOBAL BASE probable bug here
# https://github.com/emscripten-core/emscripten/blob/ac676d5e437525d15df5fd46bc2c208ec6d376a3/tools/link.py#L1652-L1658


if python3 -V 2>/dev/null
then
    echo using installed python3
else
    echo will use python for build as system python.
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



