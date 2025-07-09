# this is to be sourced by extra/*sh
export PG_EXTRA=${PG_EXTRA:-build/extra}
mkdir -p $PG_EXTRA

if $WASI
then
    if which wasi-c
    then
        echo -n
    else
        reset
        . ${SDKROOT:-/tmp/sdk}/wasm32-wasi-shell.sh
    fi
else

    if which emcc
    then
        echo -n
    else
        reset
        . ${SDKROOT:-/tmp/sdk}/wasm32-bi-emscripten-shell.sh
    fi
fi

export PGROOT=${PGROOT:-/tmp/pglite}
export PATH=${PGROOT}/bin:$PATH
. ${PGROOT}/pgopts.sh

