# wasm-build/pack_extension.py

# TODO: use this file for merging symbols too.

# use recorded file list in ${PGROOT}/pg.installed
# get other files into a tarball, find a .so and named everything after it


import asyncio
import tarfile
import os
import sys
from pathlib import Path

class Error(Exception):
    pass

def gather(root: Path, *kw):

    for current, dirnames, filenames in os.walk(root):
        rel = Path("/").joinpath(Path(current).relative_to(root))

        # print(rel, len(dirnames), len(filenames))
        yield rel, filenames

DIRS=[]
def pushd(d):
    global DIRS
    DIRS.append(os.getcwd())
    os.chdir(d)
    return 1

def popd():
    global DIRS
    os.chdir( DIRS.pop() )


def is_extension(path:Path, fullpath:Path):
    global EXTNAME, SYMBOLS, PGL_DIST_LINK, PGROOT
    asp = path.as_posix()

    # check .so
    if asp.startswith('/lib/postgresql/'):
        if path.suffix == ".so":
            EXTNAME = path.stem
            dumpcmd = f"{PGROOT}/bin/wasm-objdump -x {fullpath} > {PG_BUILD_DUMPS}/dump.{EXTNAME} 2>/dev/null "
            os.system(dumpcmd)

            os.system(f"OBJDUMP={PG_BUILD_DUMPS}/dump.{EXTNAME} python3 wasm-build/getsyms.py imports > {PGL_DIST_LINK}/imports/{EXTNAME}")
            with open(f"{PGL_DIST_LINK}/imports/{EXTNAME}","r") as f:
                SYMBOLS=f.readlines()

        return True

    # rpath
    if asp.startswith('/lib/'):
        return True

    if asp.startswith('/share/postgresql/extension'):
        return True




async def archive(target_folder):
    global INSTALLED, PACKLIST

    walked = []
    for folder, filenames in gather(target_folder):
        walked.append([folder, filenames])


    for folder, filenames in walked:
        for filename in filenames:
            test = Path(folder) / Path(filename)
            asp = test.as_posix()
            if (PGROOT/test).is_symlink():
                print("SYMLINK:", test)
                continue
            if asp not in INSTALLED:
                if asp.startswith('/sdk/'):
                    continue

                if asp.startswith('/base/'):
                    continue

                fp = PGROOT / asp[1:]
                if fp.is_symlink():
                    print("SYMLINK:", fp)
                    continue

                if is_extension(test, fp):
                    print(f"{EXTNAME=}", test, fp)
                    PACKLIST.append( [fp, test] )
                else:
                    print("custom:", test)


DIST = Path(os.environ.get("PG_DIST_EXT", "/tmp/sdk/dist/extensions-emsdk"))
BUILD=os.environ.get('BUILD','emscripten')
PGROOT=Path(os.environ.get('PGROOT',"/tmp/pglite"))
PGL_DIST_LINK=Path(os.environ.get('PGL_DIST_LINK', "/tmp/sdk/dist/pglite-link"))
PG_BUILD_DUMPS=Path(os.environ.get('PG_BUILD_DUMPS', "/tmp/sdk/build/dumps"))

INSTALLED = []

EXTNAME = ""
PACKLIST = []
SYMBOLS=[]

PREINST = "/plpgsql"
IS_PREINST = PREINST in sys.argv
for line in open(PGROOT / f"pg.{BUILD}.installed" ).readlines():
    asp = Path(line[1:].strip()).as_posix()
    if IS_PREINST:
        if asp.find(PREINST)>0:
            continue
    INSTALLED.append( asp )


print("="*80)
asyncio.run( archive(PGROOT) )
print("="*80)

if not EXTNAME:
    print("MAYBE ERROR: no new installed extension found, is it builtin ?")
    sys.exit(0)

print(f"""
PG installed in : {PGROOT=}


    {EXTNAME =} ({len(SYMBOLS)} imports)



""")

swd = os.getcwd()

if (not IS_PREINST) and ("builtin" not in sys.argv):
    if len(PACKLIST):
        if pushd(PGROOT):
            with tarfile.open(DIST / f"{EXTNAME}.tar", "w:") as tar:
                for fp, fn in PACKLIST:
                    print(f"{EXTNAME} : {fp} => {fn}")
                    tar.add(fn.as_posix()[1:])
                    # if "builtin" not in sys.argv:
                    os.remove(fp)
            popd()
    else:
        print(f"Nothing found to pack for {EXTNAME}, did you 'make install' ?")
else:
    print("Nothing to pack for builtin extension :", EXTNAME)
