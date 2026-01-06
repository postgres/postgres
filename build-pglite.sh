#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries built to wasm (see pglite-builder)
#############

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/install/pglite"}

# build with optimizations by default aka release
# PGLITE_CFLAGS="-sDYLINK_DEBUG=2 -g -gsource-map --no-wasm-opt -Wbad-function-cast -Wcast-function-type"
PGLITE_CFLAGS=""
if [ "$DEBUG" = true ]
then
    echo "pglite: building debug version."
    PGLITE_CFLAGS="$PGLITE_CFLAGS -g -gsource-map --no-wasm-opt --emit-symbol-map"
else
    echo "pglite: building release version."
    PGLITE_CFLAGS="$PGLITE_CFLAGS -O2"
    # we shouldn't need to do this, but there's a bug somewhere that prevents a successful build if this is set
    unset DEBUG
fi

echo "pglite: PGLITE_CFLAGS=$PGLITE_CFLAGS"

# run ./configure only if config.status is older than this file
# TODO: we should ALSO check if any of the PGLITE_CFLAGS have changed and trigger a ./configure if they did!!!
REF_FILE="build-pglite.sh"
CONFIG_STATUS="config.status"
RUN_CONFIGURE=false

if [ ! -f "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS does not exist, need to run ./configure"
    RUN_CONFIGURE=true
elif [ "$REF_FILE" -nt "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS is older than $REF_FILE. Need to run ./configure."
    RUN_CONFIGURE=true
else
    echo "$CONFIG_STATUS exists and is newer than $REF_FILE. ./configure will NOT be run."
fi

# we define here "all" emscripten flags in order to allow native builds (like libpglite)
EXPORTED_RUNTIME_METHODS="addFunction,removeFunction,FS,MEMFS,wasmTable"
PGLITE_EMSCRIPTEN_FLAGS="-sWASM_BIGINT \
-sSUPPORT_LONGJMP=emscripten \
-sFORCE_FILESYSTEM=1 \
-sNO_EXIT_RUNTIME=0 -sENVIRONMENT=node,web,worker \
-sMAIN_MODULE=2 -sMODULARIZE=1 -sEXPORT_ES6=1 \
-sEXPORT_NAME=Module -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH \
-sERROR_ON_UNDEFINED_SYMBOLS=0 \
-sEXPORTED_RUNTIME_METHODS=$EXPORTED_RUNTIME_METHODS \
-sTOTAL_MEMORY=32MB \
--embed-file $(pwd)/other/PGPASSFILE@/home/web_user/.pgpass"

# Step 1: configure the project
if [ "$RUN_CONFIGURE" = true ]; then
    LDFLAGS="-sWASM_BIGINT -sUSE_PTHREADS=0" \
    LDFLAGS_SL="-sSIDE_MODULE=1" \
    LDFLAGS_EX=$PGLITE_EMSCRIPTEN_FLAGS \
    CFLAGS="${PGLITE_CFLAGS} -sWASM_BIGINT -fpic -sENVIRONMENT=node,web,worker -sSUPPORT_LONGJMP=emscripten -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types" emconfigure ./configure ac_cv_exeext=.js --host aarch64-unknown-linux-gnu --disable-spinlocks --disable-largefile --without-llvm  --without-pam --disable-largefile --with-openssl=no --without-readline --without-icu --with-includes=$INSTALL_PREFIX/include:$INSTALL_PREFIX/include/libxml2:$(pwd)/pglite/includes --with-libraries=$INSTALL_PREFIX/lib --with-uuid=ossp --with-zlib --with-libxml --with-libxslt --with-template=emscripten --prefix=$INSTALL_FOLDER || { echo 'error: emconfigure failed' ; exit 11; }
else
    echo "Warning: configure has not been run because RUN_CONFIGURE=${RUN_CONFIGURE}"
fi

# Step 2: make and install all except pglite
emmake make PORTNAME=emscripten -j || { echo 'error: emmake make PORTNAME=emscripten -j' ; exit 21; }
emmake make PORTNAME=emscripten install || { echo 'error: emmake make PORTNAME=emscripten install' ; exit 22; }

# Step 3.1: make all contrib extensions - do not install

# Step 3.1.2 all the rest of contrib
emmake make PORTNAME=emscripten -C contrib/ -j || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ -j' ; exit 31; }
# Step 3.2: make dist contrib extensions - this will create an archive for each extension
emmake make PORTNAME=emscripten -C contrib/ dist || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ dist' ; exit 32; }
# the above will also create a file with the imports that each extension needs - we pass these as input in the next step for emscripten to keep alive


# Step 4: make and dist other extensions
SAVE_PATH=$PATH
PATH=$PATH:$INSTALL_FOLDER/bin
emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite || { echo 'error: emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 41; }
emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/ dist || { echo 'error: make OPTFLAGS="" PORTNAME=emscripten -C pglite/ dist ' ; exit 42; }
PATH=$SAVE_PATH

# Step 5: make and install pglite
EXPORTED_RUNTIME_METHODS="MEMFS,IDBFS,FS,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack,addFunction,removeFunction,wasmTable"
PGLITE_EMSCRIPTEN_FLAGS="-sWASM_BIGINT \
-sSUPPORT_LONGJMP=emscripten \
-sFORCE_FILESYSTEM=1 \
-sNO_EXIT_RUNTIME=1 -sENVIRONMENT=node,web,worker \
-sMAIN_MODULE=2 -sMODULARIZE=1 -sEXPORT_ES6=1 \
-sEXPORT_NAME=Module -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH \
-sERROR_ON_UNDEFINED_SYMBOLS=0 \
-sEXPORTED_RUNTIME_METHODS=$EXPORTED_RUNTIME_METHODS"
# Building pglite itself needs to be the last step because of the PRELOAD_FILES parameter (a list of files and folders) need to be available.
PGLITE_CFLAGS="$PGLITE_CFLAGS $PGLITE_EMSCRIPTEN_FLAGS" emmake make PORTNAME=emscripten -j -C src/backend/ install-pglite || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 51; }

# Step 3.1.1 pgcrypto - special case
cd ./pglite/ && ./build-pgcrypto.sh && cd ../
PGLITE_WITH_PGCRYPTO=1 emmake make PORTNAME=emscripten -C contrib/ dist 
# hack for pgcrypto. since we're linking lssl and lcrypto directly to the extension, their respective symbols should not be exported
# find / -name pgcrypto.imports -exec rm -f {} \;