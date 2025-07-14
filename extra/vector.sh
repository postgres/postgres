#!/bin/bash

. wasm-build/extension.sh

pushd $PG_EXTRA
    # [ -d pgvector ] || git clone --no-tags --depth 1 --single-branch --branch master https://github.com/pgvector/pgvector

    if [ -d vector ]
    then
        echo using local pgvector
    else
        wget -c -q https://github.com/pgvector/pgvector/archive/refs/tags/v0.8.0.tar.gz -Opgvector.tar.gz
        tar xvfz pgvector.tar.gz
        mv pgvector-?.?.? vector
    fi
popd

pushd $PG_EXTRA/vector
    # path for wasm-shared already set to (pwd:pg build dir)/bin
    # OPTFLAGS="" turns off arch optim (sse/neon).
    PG_CONFIG=${PGROOT}/bin/pg_config emmake make OPTFLAGS="" install || exit 21

    cp sql/vector--0.8.0.sql ${PGROOT}/share/postgresql/extension/
    rm ${PGROOT}/share/postgresql/extension/vector--?.?.?--?.?.?.sql
popd


