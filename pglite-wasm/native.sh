#!/bin/bash

[ -f /alpine ] && /sbin/apk add clang gcc

mkdir -p ${SDKROOT}/src ${SDKROOT}/native
if [ -d ${SDKROOT}/src/w2c2 ]
then
    echo " using local w2c2"
else
    pushd ${SDKROOT}/src
        git clone https://github.com/pygame-web/w2c2
        cp -R w2c2/w2c2/w2c2_base.h ${SDKROOT}/native/w2c2/
        cp -R w2c2/wasi ${SDKROOT}/native/
    popd
fi

mv ${PGROOT}/bin/pglite.wasi pglite.wasi
${SDKROOT}/native/w2c2/w2c2 -f 0 -t 1 -p pglite.wasi pglite.c
mv pglite.wasi ${PGL_DIST_NATIVE}/

cat > tmp.py <<END
import os
print("="*70)
print(" writing loader ....")
print()

def makefunc(l):
    global defines
    l = l.rstrip('\\n {')
    rt, fn = l.split(' ${WASM2C}_', 1)
    fn, argv = fn.rstrip(' )').split('(',1)
    argv = argv.split(',')
    args = []

    vars = []
    for i, arg in enumerate(argv):
        if not i:
            args.append( "&instance" )
            continue
        vars.append( arg.strip() )
        argtype, name  = vars[-1].split(' ')
        args.append(name)

    if vars:
        vars.append('')

    argv.pop(0)

    rtblock = ""
    if rt!='void':
        rtblock = f"{rt} rv = "
    body = f"""
__attribute__((export_name("{fn}"))) PyObject *
{fn}(PyObject *self, PyObject *args, PyObject *kwds) {{
    puts("calling {fn} Begin");
    {(";"+chr(0xd)).join(vars)}
    {rtblock}${WASM2C}_{fn}({','.join(args)});
    //
    puts("calling {fn} End");
    Py_RETURN_NONE;
}}

"""

    defines.write(f'''        {{"{fn}", (PyCFunction)${WASM2C}_{fn}, METH_VARARGS | METH_KEYWORDS, "doc_{fn}"}},
''')

    return body

with open('${WASM2C}.def','w') as defines:
    with open('${WASM2C}.pymod','w') as bodies:
        defines.write('''
        {"info", (PyCFunction)${WASM2C}_info, METH_VARARGS | METH_KEYWORDS, "info"},
        {"Begin", (PyCFunction)Begin, METH_VARARGS | METH_KEYWORDS, "Begin ${WASM2C}"},
        {"End", (PyCFunction)End, METH_VARARGS | METH_KEYWORDS, "End ${WASM2C}"},
''')

        with open('${WASM2C}.c', 'r') as source:
            for l in source.readlines():
                if l.find('${WASM2C}_')>0:
                    try:
                        bodies.write( makefunc(l) )
                    except Exception as e:
                        print("?", l, e)
                        continue

print("="*70)
with open('${WORKSPACE}/pglite-${PG_BRANCH}/pglite-modpython.c','r') as source:
    with open('tmp.c','w') as out:
        out.write( source.read().replace('\${WASM2C}','${WASM2C}') )
END

python3 tmp.py

echo " building loader ..."


PYLD="-lm"
PYEXT=""

if echo $CC|grep gcc
then
    COPTS="-O0 -g0"
    CCOPTS="-Wno-attributes"
else
    COPTS="-O0 -g0"
    CCOPTS="-fbracket-depth=4096 -Wno-unknown-attributes"
fi

COMPILE="$CC -fPIC $PYINC $COPTS $CCOPTS -I${SDKROOT}/native -I${SDKROOT}/native/w2c2 -o ${WASM2C}$PYEXT tmp.c ${SDKROOT}/native/wasi/libw2c2wasi.a $PYLD -lc"
echo $COMPILE

PYVER=$($PYTHON -V|cut -d' ' -f2|cut -d. -f1-2)
PYVER=${PYVER}$(${PYTHON}-config --abiflags)

PYINC="-D__PYDK__=1 -shared $(${PYTHON}-config --includes)"
PYEXT=$(${PYTHON}-config --extension-suffix)
PYLD="-lpython$PYVER $(${PYTHON}-config --ldflags)"

echo "
========================================================================
WASM2C=$WASM2C
COPTS=$COPTS

PYVER=$PYVER
PYEXT=$PYEXT

PYINC=$PYINC
PYLD=$PYLD

PGL_BUILD_NATIVE=${PGL_BUILD_NATIVE}
PGL_DIST_NATIVE=${PGL_DIST_NATIVE}
TARGET: ${WASM2C}$PYEXT
========================================================================
"

$COMPILE


[ -f ${WASM2C}$PYEXT ] && rm ${WASM2C}$PYEXT

# -I${SDKROOT}/src/w2c2/w2c2

COMPILE="$CC -fPIC -Os -g0 $PYINC $CCOPTS -I${SDKROOT}/native -I${SDKROOT}/native/w2c2 -o ${WASM2C}$PYEXT tmp.c ${SDKROOT}/native/wasi/libw2c2wasi.a $PYLD -lc"
echo $COMPILE

time $COMPILE

if [ -f ${WASM2C} ]
then
    du -hs ${WASM2C}
    # ./${WASM2C} $@
else
    echo build native ${WASM2C} failed
fi

PY=$(command -v python${PYMAJOR}.${PYMINOR})

echo "__________________________________________"
echo $PY
echo "__________________________________________"



if [ -f ${WASM2C}$PYEXT ]
then
    env -i $PY <<END
import sys
sys.path.append('.')

import ${WASM2C}
print(f" {${WASM2C}.info()=} ")

print("======================================================")
${WASM2C}.Begin()
print("___ 183 _____")
#print('initdb=', ${WASM2C}.pg_initdb() )
print("___ 185 ____")
${WASM2C}.End()
print("======================================================")

print("bye")
END

else
    echo "building native python module failed"
    exit 176
fi

