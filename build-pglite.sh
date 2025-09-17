#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries built to wasm (see pglite-builder)
#############

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/install/pglite"}

# build with optimizations by default aka release
PGLITE_CFLAGS="-O2"
if [ "$DEBUG" = true ]
then
    echo "pglite: building debug version."
    PGLITE_CFLAGS="-g -gsource-map --no-wasm-opt"
else
    echo "pglite: building release version."
    # we shouldn't need to do this, but there's a bug somewhere that prevents a successful build if this is set
    unset DEBUG
fi

echo "pglite: PGLITE_CFLAGS=$PGLITE_CFLAGS"

# Step 1: configure the project
LDFLAGS="-sWASM_BIGINT -sUSE_PTHREADS=0" CFLAGS="${PGLITE_CFLAGS} -sWASM_BIGINT -fpic -sENVIRONMENT=node,web,worker -sSUPPORT_LONGJMP=emscripten -DPYDK=1 -DCMA_MB=12 -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types" emconfigure ./configure ac_cv_exeext=.cjs --disable-spinlocks --disable-largefile --without-llvm  --without-pam --disable-largefile --with-openssl=no --without-readline --without-icu --with-includes=$INSTALL_PREFIX/include:$INSTALL_PREFIX/include/libxml2 --with-libraries=$INSTALL_PREFIX/lib --with-uuid=ossp --with-zlib --with-libxml --with-libxslt --with-template=emscripten --prefix=$INSTALL_FOLDER || { echo 'error: emconfigure failed' ; exit 11; }

# Step 2: make and install all except pglite
emmake make PORTNAME=emscripten -j || { echo 'error: emmake make PORTNAME=emscripten -j' ; exit 21; }
emmake make PORTNAME=emscripten install || { echo 'error: emmake make PORTNAME=emscripten install' ; exit 22; }

# Step 3.1: make all contrib extensions - do not install
emmake make PORTNAME=emscripten LDFLAGS_SL="-sSIDE_MODULE=1" -C contrib/ -j || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ -j' ; exit 31; }
# Step 3.2: make dist contrib extensions - this will create an archive for each extension
emmake make PORTNAME=emscripten -C contrib/ dist || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ dist' ; exit 32; }
# the above will also create a file with the imports that each extension needs - we pass these as input in the next step for emscripten to keep alive

# Step 4: make and dist other extensions
SAVE_PATH=$PATH
PATH=$PATH:$INSTALL_FOLDER/bin
emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite || { echo 'error: emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 41; }
emmake make OPTFLAGS="" PORTNAME=emscripten LDFLAGS_SL="-sSIDE_MODULE=1" -C pglite/ dist || { echo 'error: make OPTFLAGS="" PORTNAME=emscripten LDFLAGS_SL="-sSIDE_MODULE=1" -C pglite/ dist ' ; exit 42; }
PATH=$SAVE_PATH

# Step 5: make and install pglite
# Building pglite itself needs to be the last step because of the PRELOAD_FILES parameter (a list of files and folders) need to be available.
PGLITE_CFLAGS=$PGLITE_CFLAGS emmake make PORTNAME=emscripten -j -C src/backend/ install-pglite || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 51; }
