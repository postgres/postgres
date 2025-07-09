#!/usr/bin/env python3
import sys
import os

def dbg(*argv, **kw):
    kw.setdefault('file',sys.stderr)
    return print(*argv,**kw)

if "${WORKSPACE}".endswith('{WORKSPACE}'):
    with open("/data/git/pglite-16.x/patches/exports.pglite", "r") as file:
        exports  = set(map(str.strip, file.readlines()))
else:
    with open("${WORKSPACE}/patches/exports.pglite", "r") as file:
        exports  = set(map(str.strip, file.readlines()))

with open("/tmp/symbols", "r") as file:
    imports  = set(map(str.strip, file.readlines()))

matches = list( imports.intersection(exports) )
for sym in matches:
    print(sym)

dbg(f"""
exports {len(exports)}
imports {len(imports)}
Matches : {len(matches)}
""")
