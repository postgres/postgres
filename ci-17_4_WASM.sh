#!/bin/bash
export WORKSPACE=$(pwd)
export PG_VERSION=${PG_VERSION:-17.4}
export PG_BRANCH=${PG_BRANCH:-REL_17_4_WASM}
export CONTAINER_PATH=${CONTAINER_PATH:-/tmp/fs}
export DEBUG=${DEBUG:-false}
export USE_ICU=${USE_ICU:-false}


PG_DIST_EXT="${WORKSPACE}/postgresql/dist/extensions-emsdk"
PG_DIST_PGLITE="${WORKSPACE}/postgresql/dist/pglite-sandbox"

#============== fix for pglite CI ============

ln -s $(pwd) postgresql-${PG_BRANCH}
ln -s $(realpath ..) pglite
ln -s $(realpath ..) postgresql-${PG_BRANCH}/pglite

#=============================================


pushd postgresql-${PG_BRANCH}

    cat > $CONTAINER_PATH/portable.opts <<END
export DEBUG=${DEBUG}
export USE_ICU=${USE_ICU}
END

    ${WORKSPACE}/portable/portable.sh

    if [ -f build/postgres/libpgcore.a ]
    then
        for archive in ${PG_DIST_EXT}/*.tar
        do
            echo "    packing $archive"
            gzip -f -9 $archive
        done

        echo "

    *   preparing TS build assets

"

        if [ -d ${WORKSPACE}/pglite/packages/pglite ]
        then
            pushd ${WORKSPACE}/pglite
                # clean
                [ -d packages/pglite/release ] && rm packages/pglite/release/* packages/pglite/dist/* packages/pglite/dist/*/*

                # be safe
                mkdir -p packages/pglite/release packages/pglite/dist

                rmdir packages/pglite/dist/*

                #update
                mv -vf ${WORKSPACE}/postgresql-${PG_BRANCH}/pglite.* packages/pglite/release/
                mv -vf ${PG_DIST_EXT}/*.tar.gz packages/pglite/release/
            popd
        else
            echo "cound not find electric-sql/pglite web and typescript support"
            exit 62
        fi

        echo "
    *   compiling TS

"
        if $CI
        then
            pushd build/postgres
                echo "# packing dev files to /tmp/sdk/libpglite-emsdk.tar.gz"
                tar -cpRz libpgcore.a pglite.* > /tmp/sdk/libpglite-emsdk.tar.gz
            popd

            pushd pglite
                npm install -g pnpm vitest
                pnpm install
            popd
        fi


        pushd pglite
            if pnpm run ts:build
            then
                pushd packages/pglite
                    if $CI
                    then
                        echo -n
                    fi
                popd
            fi
        popd

        ./runtests.sh

    else
        echo failed to build libpgcore static
        exit 125
    fi
popd

