# this only runs when wasm-objdump is working and OBJDUMP not set to false

echo "============= link export : begin ==============="

# -O0 -sEXPORT_ALL should make all symbols visible without any mangling.
COPT="-O0 -g3" \
 emcc \
 $EMCC_WEB -fPIC -sMAIN_MODULE=1 -sEXPORT_ALL -sERROR_ON_UNDEFINED_SYMBOLS -sASSERTIONS=0 \
 -DPREFIX=${PGROOT} -lnodefs.js -lidbfs.js \
 -sEXPORTED_RUNTIME_METHODS=FS,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack,ccall,cwrap,callMain \
 $PGPRELOAD \
 -o postgres.html $PG_O $PG_L || exit 14

echo "FULL:" > ${WORKSPACE}/build/sizes.log
du -hs postgres.wasm >> ${WORKSPACE}/build/sizes.log
echo >> ${WORKSPACE}/build/sizes.log


echo "getting wasm exports lists"
wasm-objdump -x $(realpath postgres.wasm) > ${WORKSPACE}/patches/exports/pgcore.wasm-objdump


pushd ${WORKSPACE}
    echo "getting postgres exports lists"
    cat $(find build/postgres -type f |grep /exports) \
     | grep -v ^\ local \
     | grep -v ^{\ global \
     | sort | uniq > ${WORKSPACE}/patches/exports/pgcore.exports

    echo "
    Merging wasm pg core symbols and postgres exports lists
     into ${WORKSPACE} patches/exports/pgcore
"

    OBJDUMP=patches/exports/pgcore.wasm-objdump \
     PGDUMP=patches/exports/pgcore.exports \
     python3 wasm-build/getsyms.py exports > patches/exports/pgcore
popd

echo "============= link export : end ==============="

