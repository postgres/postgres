#!/bin/bash

SKIP_CONTRIB=${SKIP_CONTRIB:-false}

mkdir -p ${PGL_DIST_LINK}/exports ${PGL_DIST_LINK}/imports
cd ${WORKSPACE}


if $WASI
then
    echo "

    * WASI: skipping some contrib extensions build

    "
        echo " ========= TODO WASI openssl ============== "
        SKIP="\
 [\
 sslinfo bool_plperl hstore_plperl hstore_plpython jsonb_plperl jsonb_plpython\
 ltree_plpython sepgsql bool_plperl start-scripts\
 pgcrypto uuid-ossp xml2\
 ]"
    SKIP_CONTRIB=true
else

    # TEMP FIX for SDK
    SSL_INCDIR=$EMSDK/upstream/emscripten/cache/sysroot/include/openssl
    [ -f $SSL_INCDIR/evp.h ] || ln -s $PREFIX/include/openssl $SSL_INCDIR
    SKIP="\
 [\
 sslinfo bool_plperl hstore_plperl hstore_plpython jsonb_plperl jsonb_plpython\
 ltree_plpython sepgsql bool_plperl start-scripts\
 ]"

fi

# common wasi/emsdk contrib build
if $SKIP_CONTRIB
then
    echo "
    * skipping contrib build
    "
    exit 0
fi

for extdir in postgresql-${PG_BRANCH}/contrib/*
do

    if [ -d "$extdir" ]
    then
        ext=$(echo -n $extdir|cut -d/ -f3)
        if echo -n $SKIP|grep -q "$ext "
        then
            echo "

    skipping extension $ext

            "
        else
            echo "

    Building contrib extension : $ext : begin

            "
            pushd ${BUILD_PATH}/contrib/$ext
            if PATH=$PREFIX/bin:$PATH emmake make install 2>&1 >/dev/null
            then
                echo "

    Building contrib extension : $ext : end

            "
            else
                echo "

    Extension $ext from $extdir failed to build

                "
                exit 69
            fi
            popd

            python3 ${PORTABLE}/pack_extension.py 2>&1 >/dev/null

        fi
    fi
done




echo "

        Extensions distribution folder : ${PG_DIST_EXT}


"

#        if $CI
#        then
#            #if [ -d $PREFIX/include/X11 ]
#            if true
#            then
#                echo -n
#            else
#                # install EXTRA sdk
#                . /etc/lsb-release
#                DISTRIB="${DISTRIB_ID}-${DISTRIB_RELEASE}"
#                CIVER=${CIVER:-$DISTRIB}
#                SDK_URL=https://github.com/pygame-web/python-wasm-sdk-extra/releases/download/$SDK_VERSION/python3.13-emsdk-sdk-extra-${CIVER}.tar.lz4
#                echo "Installing extra lib from $SDK_URL"
#                curl -sL --retry 5 $SDK_URL | tar xvP --use-compress-program=lz4 | pv -p -l -s 15000 >/dev/null
#                chmod +x ./extra/*.sh
#            fi
#        fi

if [ -f ${PG_BUILD_DUMPS}/dump.vector ]
then
    echo "

    *   NOT rebuilding extra extensions ( found ${PG_BUILD_DUMPS}/dump.vector )

"
else

    for extra_ext in ./extra/*.sh
    do
        LOG=$PG_DIST_EXT/$(basename ${extra_ext}).log
        echo "====  ${extra_ext} : $LOG ===="

        ${extra_ext} > $LOG || exit 112

        python3 wasm-build/pack_extension.py
    done


fi
