#!/bin/bash

. wasm-build/extension.sh

pushd $PG_EXTRA
    if [ -d pg_uuidv7 ]
    then
        echo using local pg_uuidv7
    else
        wget https://github.com/fboulnois/pg_uuidv7/archive/refs/tags/v1.6.0.tar.gz -O-|tar xfz -
        mv pg_uuidv7-1.*.* pg_uuidv7
        if $WASI
        then
            echo "no patching"
        else
            echo "PATCH?"

        fi
    fi
popd

pushd $PG_EXTRA/pg_uuidv7
    PG_CONFIG=${PGROOT}/bin/pg_config emmake make OPTFLAGS="" install || exit 25
popd

