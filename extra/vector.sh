#!/bin/bash

mkdir -p build src

pushd build
    # [ -d pgvector ] || git clone --no-tags --depth 1 --single-branch --branch master https://github.com/pgvector/pgvector

    if [ -d vector ]
    then
        echo using local pgvector
    else
        [ -f ../src/pgvector.tar.gz ] || wget -c -q https://github.com/pgvector/pgvector/archive/refs/tags/v0.8.0.tar.gz -O../src/pgvector.tar.gz
        tar xvfz ../src/pgvector.tar.gz
        mv pgvector-?.?.? vector
    fi
popd



if which emcc
then
    echo -n
else
    reset;
    . /opt/python-wasm-sdk/wasm32-bi-emscripten-shell.sh
    export PGROOT=${PGROOT:-/tmp/pglite}
    export PATH=${PGROOT}/bin:$PATH
fi


pushd build/vector
    # path for wasm-shared already set to (pwd:pg build dir)/bin
    # OPTFLAGS="" turns off arch optim (sse/neon).
    PG_CONFIG=${PGROOT}/bin/pg_config emmake make OPTFLAGS="" install || exit 33

    cp sql/vector.sql sql/vector--0.8.0.sql ${PGROOT}/share/postgresql/extension/
    rm ${PGROOT}/share/postgresql/extension/vector--?.?.?--?.?.?.sql ${PGROOT}/share/postgresql/extension/vector.sql
popd


