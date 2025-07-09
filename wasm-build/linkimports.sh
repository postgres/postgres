echo "============= link imports : begin ==============="

# TODO : make a C-API list
# _main,_getenv,_setenv,_interactive_one,_interactive_write,_interactive_read,_pg_initdb,_pg_shutdown


# extract own pg lib requirements
mkdir -p ${PGL_DIST_LINK}/imports

pushd ${WORKSPACE}
    > ${PGL_DIST_LINK}/imports/pgcore

    for extra_pg_so in $(find $PGROOT/lib/postgresql/|grep \.so$)
    do
        SOBASE=${PG_BUILD_DUMPS}/$(basename $extra_pg_so .so)
        wasm-objdump -x $(realpath $extra_pg_so) > $SOBASE.wasm-objdump
        OBJDUMP=$SOBASE.wasm-objdump \
         PGDUMP=${PGL_DIST_LINK}/exports/pgcore.exports \
         python3 wasm-build/getsyms.py imports >> ${PGL_DIST_LINK}/imports/pgcore
    done
popd

#not yet

#_emscripten_copy_from
#_emscripten_copy_to
#_emscripten_copy_to_end

# copyFrom,copyTo,copyToEnd
    cat ${PGL_DIST_LINK}/imports/* | sort | uniq > /tmp/symbols

    echo "Requesting $(wc -l /tmp/symbols) symbols from pg core for PGlite extensions"



    python3 <<END > ${PGL_DIST_LINK}/exports/pglite

import sys
import os

def dbg(*argv, **kw):
    kw.setdefault('file',sys.stderr)
    return print(*argv,**kw)

with open("${PGL_DIST_LINK}/exports/pgcore", "r") as file:
    exports  = set(map(str.strip, file.readlines()))

with open("/tmp/symbols", "r") as file:
    imports  = set(map(str.strip, file.readlines()))

matches = list( imports.intersection(exports) )


# ?
for sym in """

_clear_error
_get_buffer_addr
_get_buffer_size
_get_channel
_interactive_one
_interactive_read
_interactive_write
_pgl_backend
_pgl_closed
_pgl_initdb
_pgl_shutdown
_use_wire

_main

_ErrorContext
_check_function_bodies
_clock_gettime
_CurrentMemoryContext
___cxa_throw
_error_context_stack
_getenv
_lowerstr

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

for sym in """
_PQcancelCreate
_PQcancelErrorMessage
_PQcancelFinish
_PQcancelPoll
_PQcancelSocket
_PQcancelStart
_PQclear
_PQcmdStatus
_PQconnectPoll
_PQconnectStartParams
_PQconnectionUsedPassword
_PQconsumeInput
_PQerrorMessage
_PQfinish
_PQgetResult
_PQgetisnull
_PQgetvalue
_PQisBusy
_PQnfields
_PQntuples
_PQresultErrorField
_PQresultStatus
_PQsendQuery
_PQserverVersion
_PQsetSingleRowMode
_PQsocket
_PQstatus
_pgresStatus

_PQbackendPID
_PQconninfo
_PQconninfoFree
_PQconninfoParse
_PQendcopy
_PQescapeIdentifier
""".splitlines():
    if sym and sym in matches:
        dbg(f"\t* Removed symbol '{sym}'")
        matches.remove(sym)

# matches.append('')

if not '_getTempRet0' in matches:
    matches.append('_getTempRet0')
if not 'getTempRet0' in matches:
    matches.append('getTempRet0')

for sym in matches:
    print(sym)

dbg(f"""
exports {len(exports)}
imports {len(imports)}
Matches : {len(matches)}
""")
END

echo "============= link imports : end ==============="
