if [ -f postgresql/postgresql-${PG_VERSION}.patched ]
then
    echo version already selected and patch stage already done
else
    if [ -f configure ]
    then
        echo "building in tree ( docker or proot )"
        ln -s $(pwd) postgresql-${PG_VERSION}
    else
        git clone --no-tags --depth 1 --single-branch --branch ${PG_VERSION} https://github.com/electric-sql/postgres-pglite postgresql-${PG_VERSION}
    fi

    if pushd postgresql-${PG_VERSION}
    then
            echo
        touch ./src/template/emscripten
        touch ./src/include/port/emscripten.h
        touch ./src/include/port/wasm_common.h
        touch ./src/makefiles/Makefile.emscripten
        for patchdir in \
            postgresql-debug \
            postgresql-emscripten \
            postgresql-pglite
        do
            if [ -d ../patches/$patchdir ]
            then
                for one in ../patches/$patchdir/*.diff
                do
                    if cat $one | patch -p1
                    then
                        echo applied $one
                    else
                        echo "

Fatal: failed to apply patch : $one
"
                        exit 30
                    fi
                done
            fi
        done
        touch postgresql-${PG_VERSION}.patched
        popd
    fi

    # either a submodule dir or a symlink.
    # currently release only use symlink
    [ -f postgresql/configure ] && rm postgresql 2>/dev/null

    # do nothing if it is a submodule
    [ -d postgresql ] || ln -s $(realpath postgresql-${PG_VERSION}) postgresql

fi

export PGSRC=$(realpath postgresql)

echo "

Building $ARCHIVE (patched) from $PGSRC WASI=$WASI


build-pgcore:begin
___________________________________________________

CC_PGLITE=$CC_PGLITE


"

if [ -f ${PGROOT}/pg.installed ]
then
    echo "  *  skipping pg build, using previous install from ${PGROOT}"
else

    mkdir -p build/postgres
    pushd build/postgres

    # create empty package.json to avoid emsdk node conflicts
    # with root package.json of project
    echo "{}" > package.json


    if $CI
    then
        echo "CI : using build cache"
    else
        if [ -f Makefile ]
        then
            echo "Cleaning up previous build ..."
            make distclean 2>&1 > /dev/null
        fi
    fi

# TODO: --with-libxml    xml2 >= 2.6.23
# TODO: --with-libxslt   add to sdk
#  --disable-atomics https://github.com/WebAssembly/threads/pull/147  "Allow atomic operations on unshared memories"


    COMMON_CFLAGS="${CC_PGLITE} -fpic -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types"

    if ${WASI}
    then
        BUILD=wasi
        echo "WASI BUILD: turning off xml/xslt support"
        XML2=""
        UUID=""

        cp ${PORTABLE}/wasi/wasi_port.c /tmp/pglite/include/sdk_port.c
        WASM_LDFLAGS="-lwasi-emulated-getpid -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks"
        WASM_CFLAGS="-DSDK_PORT=/tmp/pglite/include/sdk_port.c ${COMMON_CFLAGS} -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"
        export MAIN_MODULE=""

    else
        BUILD=emscripten

        # --with-libxml does not fit with --without-zlib
        if $CI
        then
            # do not build obsolete ext xml2 on CI
            XML2="--with-zlib --with-libxml"
        else
            XML2="--with-zlib --with-libxml --with-libxslt"
        fi
        UUID="--with-uuid=ossp"
        WASM_CFLAGS="${COMMON_CFLAGS}"
        WASM_LDFLAGS="-sERROR_ON_UNDEFINED_SYMBOLS"
        export MAIN_MODULE="-sMAIN_MODULE=1"
    fi

    export XML2_CONFIG=$PREFIX/bin/xml2-config

    if $USE_ICU
    then
        CNF_ICU="--with-icu"
    else
        CNF_ICU="--without-icu"
    fi


    cp ${PGSRC}/src/include/port/wasm_common.h /tmp/pglite/include/wasm_common.h

    [ -f ${PREFIX}/devices/emsdk/usr/lib/libossp-uuid.a ] && rm ${PREFIX}/devices/emsdk/usr/lib/libossp-uuid.a
    [ -f ${PREFIX}/devices/emsdk/usr/lib/libouuid.a ] && rm ${PREFIX}/devices/emsdk/usr/lib/libuuid.a


    CNF="${PGSRC}/configure --prefix=${PGROOT} --cache-file=${PGROOT}/config.cache.${BUILD} \
 --disable-spinlocks --disable-largefile --without-llvm \
 --without-pam --disable-largefile --with-openssl=no \
 --without-readline $CNF_ICU \
 ${UUID} ${XML2} ${PGDEBUG}"


    mkdir -p bin

    GETZIC=${GETZIC:-true}

    EMCC_NODE="-sEXIT_RUNTIME=1 -DEXIT_RUNTIME -sNODERAWFS -sENVIRONMENT=node"


    if $WASI
    then

        export EXT=wasi
        cat > ${PGROOT}/config.site <<END
ac_cv_exeext=.wasi
END
        if $GETZIC
        then
            cat > bin/zic <<END
#!/bin/bash
#. ${SDKROOT}/wasm32-wasi-shell.sh
TZ=UTC PGTZ=UTC $(command -v wasi-run) $(pwd)/src/timezone/zic.wasi \$@
END
	else
 		echo "
   * Using system ZIC from ${ZIC:-/usr/sbin/zic}
   "
 		cp ${ZIC:-/usr/sbin/zic} bin/
        fi

    else
        export EXT=wasm
        cat > ${PGROOT}/config.site <<END
ac_cv_exeext=.cjs
END

        if $GETZIC
        then
            cat > bin/zic <<END
#!/bin/bash
#. ${SDKROOT}/wasm32-bi-emscripten-shell.sh
TZ=UTC PGTZ=UTC $(command -v node) $(pwd)/src/timezone/zic.cjs \$@
END
	else
 		echo "
   * Using system ZIC from ${ZIC:-/usr/sbin/zic}
   "
 		cp ${ZIC:-/usr/sbin/zic} bin/
        fi
    fi

    export ZIC=$(realpath bin/zic)

    if \
     EM_PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
     CONFIG_SITE="${PGROOT}/config.site" \
     CFLAGS="$WASM_CFLAGS" \
     LDFLAGS="$WASM_LDFLAGS" \
     emconfigure $CNF --with-template=$BUILD
    then
        echo configure ok
    else
        echo configure failed
        exit 194
    fi

    echo "



    =============================================================

    building $BUILD wasm MVP:$MVP Debug=${PGDEBUG} with :

    opts : $@

    COPTS=$COPTS
    LOPTS=$LOPTS


    PYDK_CFLAGS=$PYDK_CFLAGS

    EMCC_CFLAGS=$EMCC_NODE

    CFLAGS=$WASM_CFLAGS

    LDFLAGS=$WASM_LDFLAGS

    PG_DEBUG_HEADER=$PG_DEBUG_HEADER

    ZIC=$ZIC

    ===============================================================




    "


    if grep -q MAIN_MODULE ${PGSRC}/src/backend/Makefile
    then
        echo "dyld server patch ok"
    else
        echo "missing server dyld patch"
        exit 236
    fi

    if $WASI
    then
        sed -i 's|-pthread||g' ./src/Makefile.global
    fi


    # --disable-shared not supported so be able to use a fake linker

    > /tmp/disable-shared.log

    mkdir -p $PGROOT/bin

    cat > $PGROOT/bin/emsdk-shared <<END
#!/bin/bash
echo "[\$(pwd)] $0 \$@" >> /tmp/disable-shared.log
# shared build
COPTS="$LOPTS" \${PG_LINK:-emcc} -L${PREFIX}/lib -DPREFIX=${PGROOT} -shared -sSIDE_MODULE=1 \$@ -Wno-unused-function
END
    ln -sf $PGROOT/bin/emsdk-shared bin/emsdk-shared


    cat > $PGROOT/bin/wasi-shared <<END
#!/bin/bash
echo "[\$(pwd)] $0 \$@" >> /tmp/disable-shared.log
# shared build
echo ===================================================================================
wasi-c -L${PREFIX}/lib -DPREFIX=${PGROOT} -shared \$@ -Wno-unused-function
echo ===================================================================================
END
    ln -sf $PGROOT/bin/wasi-shared bin/wasi-shared

    chmod +x bin/zic $PGROOT/bin/wasi-shared $PGROOT/bin/emsdk-shared

    # for zic and emsdk-shared/wasi-shared called from makefile
    export PATH=$(pwd)/bin:$PATH

> /tmp/build.log
#  2>&1 > /tmp/build.log
    if $DEBUG
    then
        NCPU=1
    else
        NCPU=$(nproc)
    fi

    # Get rid of some build stages for now

    cat > src/test/Makefile <<END
# auto-edited for pglite
all: \$(echo src/test and src/test/isolation skipped)
clean check installcheck all-src-recurse: all
install: all
END
    cat src/test/Makefile > src/test/isolation/Makefile

    # Keep a shell script for fast rebuild with env -i from cmdline

    cat > pg-make.sh <<END
#!/bin/bash
. ${SDKROOT}/wasm32-bi-emscripten-shell.sh
export PATH=$PGROOT/bin:\$PATH

# ZIC=$ZIC
# ${PGROOT}/pgopts.sh

END
    cat ${PGROOT}/pgopts.sh >> pg-make.sh

    cat >> pg-make.sh <<END

# linker stage
echo '

Linking ...

'
cat $SDKROOT/VERSION
rm -vf libp*.a src/backend/postgres*
EMCC_CFLAGS="${CC_PGLITE} ${EMCC_NODE}" emmake make AR=\${EMSDK}/upstream/bin/llvm-ar PORTNAME=emscripten $BUILD=1 -j \${NCPU:-$NCPU} \$@

echo '____________________________________________________________'
du -hs src/port/libpgport_srv.a src/common/libpgcommon_srv.a libp*.a src/backend/postgres*
echo '____________________________________________________________'

END

    chmod +x pg-make.sh

    if env -i ./pg-make.sh install
    then
        echo install ok
        if $WASI
        then
            # remove unlinked server
            for todel in src/backend/postgres src/backend/postgres.wasi $PGROOT/bin/postgres $PGROOT/bin/postgres.wasi
            do
                [ -f $todel ] && rm $todel
            done

            pushd ../..
                chmod +x ./wasm-build/linkwasi.sh
                # WASI_CFLAGS="$WASI_CFLAGS"
                ./wasm-build/linkwasi.sh || exit 251
            popd
            cp src/backend/postgres.wasi $PGROOT/bin/ || exit 253
        else
            if $DEBUG
            then
                # built with EMCC_CFLAGS="-sEXIT_RUNTIME=1 -DEXIT_RUNTIME -sNODERAWFS -sENVIRONMENT=node" emmake make -C
                cp src/bin/initdb/initdb.wasm $PGROOT/bin/

                cp src/backend/postgres.wasm $PGROOT/bin/

            fi

            mv src/bin/pg_config/pg_config.wasm ${PGROOT}/bin/
            cat > ${PGROOT}/bin/pg_config <<END
#!/bin/bash
$(which node) ${PGROOT}/bin/pg_config.cjs \$@
END
            chmod +x ${PGROOT}/bin/pg_config
        fi

        pushd ${PGROOT}

        find . -type f | grep -v plpgsql > ${PGROOT}/pg.installed
        popd

        goback=$(pwd)
        popd
        python3 wasm-build/pack_extension.py builtin
        pushd $goback

        pushd ${PGROOT}
        find . -type f  > ${PGROOT}/pg.installed
        popd

    else
        cat /tmp/install.log
        echo "install failed"
        exit 384
    fi

    python3 > ${PGROOT}/PGPASSFILE <<END
USER="${PGPASS:-postgres}"
PASS="${PGUSER:-postgres}"
md5pass =  "md5" + __import__('hashlib').md5(USER.encode() + PASS.encode()).hexdigest()
print(f"localhost:5432:postgres:{USER}:{md5pass}")

USER="postgres"
PASS="postgres"
md5pass =  "md5" + __import__('hashlib').md5(USER.encode() + PASS.encode()).hexdigest()
print(f"localhost:5432:postgres:{USER}:{md5pass}")

USER="login"
PASS="password"
md5pass =  "md5" + __import__('hashlib').md5(USER.encode() + PASS.encode()).hexdigest()
print(f"localhost:5432:postgres:{USER}:{md5pass}")
END

    # for extensions building
    chmod +x ${PGROOT}/bin/pg_config


	echo "TODO: node/wasi cmdline initdb for PGDATA=${PGDATA} "
    popd

fi

echo "build-pgcore: end




"

