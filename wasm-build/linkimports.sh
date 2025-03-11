echo "============= link imports : begin ==============="

# TODO : make a C-API list
# _main,_getenv,_setenv,_interactive_one,_interactive_write,_interactive_read,_pg_initdb,_pg_shutdown


# extract own pg lib requirements

pushd ${WORKSPACE}
    > patches/imports/pgcore

    for extra_pg_so in $(find $PGROOT/lib/postgresql/|grep \.so$)
    do
        SOBASE=patches/imports.pgcore/$(basename $extra_pg_so .so)
        wasm-objdump -x $(realpath $extra_pg_so) > $SOBASE.wasm-objdump
        OBJDUMP=$SOBASE.wasm-objdump \
         PGDUMP=patches/exports/pgcore.exports \
         python3 wasm-build/getsyms.py imports >> patches/imports/pgcore
    done
popd

#not yet

#_emscripten_copy_from
#_emscripten_copy_to
#_emscripten_copy_to_end

# copyFrom,copyTo,copyToEnd
    cat ${WORKSPACE}/patches/imports/* | sort | uniq > /tmp/symbols

    echo "Requesting $(wc -l /tmp/symbols) symbols from pg core for PGlite extensions"



    python3 <<END > ${WORKSPACE}/patches/exports/pglite

import sys
import os

def dbg(*argv, **kw):
    kw.setdefault('file',sys.stderr)
    return print(*argv,**kw)

with open("${WORKSPACE}/patches/exports/pgcore", "r") as file:
    exports  = set(map(str.strip, file.readlines()))

with open("/tmp/symbols", "r") as file:
    imports  = set(map(str.strip, file.readlines()))

matches = list( imports.intersection(exports) )

# ?
for sym in """
_ErrorContext
_check_function_bodies
_clock_gettime
_CurrentMemoryContext
___cxa_throw
_error_context_stack
_getenv
_interactive_one
_interactive_read
_interactive_write
_lowerstr
_main
_pg_initdb
_pg_shutdown
_readstoplist
_searchstoplist
_setenv
_shmem_request_hook
_shmem_startup_hook
_stderr
_TopMemoryContext
""".splitlines():
    if sym and not sym in matches:
        matches.append(sym)

matches.sort()

for sym in matches:
    print(sym)



dbg(f"""
exports {len(exports)}
imports {len(imports)}
Matches : {len(matches)}
""")
END

echo "============= link imports : end ==============="
