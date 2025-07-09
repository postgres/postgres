# this only runs when wasm-objdump is working and OBJDUMP not set to false

mkdir -p ${PGL_DIST_LINK}/exports ${PGL_DIST_LINK}/imports ${PG_BUILD_DUMPS}

echo "============= link export to ${PGL_DIST_LINK}/exports : begin ==============="

echo "FULL:" > ${PGL_DIST_LINK}/sizes.log
du -hs ${PGL_DIST_JS}/pglite-js.* >> ${PGL_DIST_LINK}/sizes.log
echo >> ${PGL_DIST_LINK}/sizes.log


echo "
    * getting wasm exports lists
"

pushd $(realpath ${PGL_DIST_JS})
    wasmtime --dir / --dir $(pwd)::. -- $(which wasm-objdump).wasi -x pglite-js.wasm > ${PG_BUILD_DUMPS}/pgcore.wasm-objdump
popd


pushd ${PGL_DIST_LINK}
    echo "
    * getting postgres exports lists from ${BUILD_PATH}
"
    cat $(find ${BUILD_PATH} -type f |grep /exports|grep -v /interfaces/libpq/) \
     | grep -v ^\ local \
     | grep -v ^{\ global \
     | sort | uniq > ${PGL_DIST_LINK}/exports/pgcore.exports

    echo "
    * Merging wasm pg core symbols and postgres exports lists
       into ${PGL_DIST_LINK}/exports/pgcore
"

    OBJDUMP=${PG_BUILD_DUMPS}/pgcore.wasm-objdump \
     PGDUMP=${PGL_DIST_LINK}/exports/pgcore.exports \
     python3 ${WORKSPACE}/wasm-build/getsyms.py exports > ${PGL_DIST_LINK}/exports/pgcore
popd

echo "============= link export : end ==============="

