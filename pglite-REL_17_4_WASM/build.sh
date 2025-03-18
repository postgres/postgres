#!/bin/bash
echo "pglite/build: begin"

WORKSPACE=$(pwd)
PGROOT=/tmp/pglite

PGSRC=${WORKSPACE}
PGBUILD=${WORKSPACE}/build/postgres

LIBPGCORE=${PGBUILD}/libpgcore.a

WEBROOT=${PGBUILD}/web

PGINC="-I/tmp/pglite/include \
 -I${PGSRC}/src/include -I${PGSRC}/src/interfaces/libpq \
 -I${PGBUILD}/src/include"


if $WASI
then
    echo TODO


else
    . ${SDKROOT:-/opt/python-wasm-sdk}/wasm32-bi-emscripten-shell.sh

    touch placeholder

    export PGPRELOAD="\
--preload-file ${PGROOT}/share/postgresql@${PGROOT}/share/postgresql \
--preload-file ${PGROOT}/lib/postgresql@${PGROOT}/lib/postgresql \
--preload-file ${PGROOT}/password@${PGROOT}/password \
--preload-file ${PGROOT}/PGPASSFILE@/home/web_user/.pgpass \
--preload-file placeholder@${PGROOT}/bin/postgres \
--preload-file placeholder@${PGROOT}/bin/initdb\
"

    export CC=$(which emcc)


    EXPORTED_FUNCTIONS="_main,_use_wire,_pgl_initdb,_pgl_backend,_pgl_shutdown,_interactive_write,_interactive_read,_interactive_one"

    EXPORTED_RUNTIME_METHODS="MEMFS,IDBFS,FS,FS_mount,FS_syncfs,FS_analyzePath,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack"
    EXPORTED_RUNTIME_METHODS="MEMFS,IDBFS,FS,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack"



    if $DEBUG
    then
        # FULL
        LINKER="-sMAIN_MODULE=1 -sEXPORTED_FUNCTIONS=${EXPORTED_FUNCTIONS}"
    else
        # min
        # LINKER="-sMAIN_MODULE=2"

        # tailored
        LINKER="-sMAIN_MODULE=2 -sEXPORTED_FUNCTIONS=@exports"
LINKER="-sMAIN_MODULE=1 -sEXPORTED_FUNCTIONS=${EXPORTED_FUNCTIONS}"
    fi

    echo "

________________________________________________________

emscripten : $(which emcc ) $(cat ${SDKROOT}/VERSION)
python : $(which python3) $(python3 -V)
wasmtime : $(which wasmtime)

CC=${CC:-undefined}

Linking to libpgcore static from $LIBPGCORE

Folders :
    source : $PGSRC
     build : $PGBUILD
    target : $WEBROOT

    CPOPTS : $COPTS
    DEBUG  : $DEBUG
        LOPTS  : $LOPTS
    CMA_MB : $CMA_MB

 CC_PGLITE : $CC_PGLITE

  ICU i18n : $USE_ICU

$PGPRELOAD
________________________________________________________



"

    rm pglite.*

    mkdir -p $WEBROOT

    if $USE_ICU
    then
        LINK_ICU="${PREFIX}/lib/libicui18n.a ${PREFIX}/lib/libicuuc.a ${PREFIX}/lib/libicudata.a"
    else
        LINK_ICU=""
    fi

#    ${CC} ${CC_PGLITE} -DPG_INITDB_MAIN \
#     ${PGINC} \
#     -o ${PGBUILD}/initdb.o -c ${PGSRC}/src/bin/initdb/initdb.c

    ${CC} ${CC_PGLITE} ${PGINC} -o ${PGBUILD}/pglite.o -c ${WORKSPACE}/pglite-wasm/pg_main.c \
     -Wno-incompatible-pointer-types-discards-qualifiers

    COPTS="$LOPTS" ${CC} ${CC_PGLITE} -sGLOBAL_BASE=${CMA_MB}MB -o pglite-rawfs.js -ferror-limit=1  \
     -sFORCE_FILESYSTEM=1 $EMCC_NODE \
         -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH -sERROR_ON_UNDEFINED_SYMBOLS \
         -sEXPORTED_RUNTIME_METHODS=${EXPORTED_RUNTIME_METHODS} \
     ${PGINC} ${PGBUILD}/pglite.o \
     $LINKER $LIBPGCORE \
     $LINK_ICU \
     -lnodefs.js -lidbfs.js -lxml2 -lz


    # some content that does not need to ship into .data
    for cleanup in snowball_create.sql psqlrc.sample
    do
        > ${PREFIX}/${cleanup}
    done

    mkdir -p ${PG_DIST:-/tmp/sdk/dist}/pglite
    COPTS="$LOPTS" ${CC} ${CC_PGLITE} -sGLOBAL_BASE=${CMA_MB}MB -o ${PG_DIST:-/tmp/sdk/dist}/pglite/pglite.html -ferror-limit=1 --shell-file ${WORKSPACE}/pglite-wasm/repl.html \
     $PGPRELOAD \
     -sFORCE_FILESYSTEM=1 -sNO_EXIT_RUNTIME=1 -sENVIRONMENT=node,web \
     -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=Module \
         -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH -sERROR_ON_UNDEFINED_SYMBOLS \
         -sEXPORTED_RUNTIME_METHODS=${EXPORTED_RUNTIME_METHODS} \
     ${PGINC} ${PGBUILD}/pglite.o \
     $LINKER $LIBPGCORE \
     $LINK_ICU \
     -lnodefs.js -lidbfs.js -lxml2 -lz

fi

du -hs ${PG_DIST:-/tmp/sdk/dist}/pglite/pglite.*

echo "pglite/build: end"
