# cibuild/pack_extension.py

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



def is_extension(path:Path, fullpath:Path):
    global EXTNAME, SYMBOLS, PGPATCH, PGROOT
    asp = path.as_posix()

    # check .so
    if asp.startswith('/lib/postgresql/'):
        if path.suffix == ".so":
            EXTNAME = path.stem
            if os.environ.get('OBJDUMP',''):
                os.system(f"wasm-objdump -x {fullpath} > {PGROOT}/dump.{EXTNAME}")
                os.system(f"OBJDUMP={PGROOT}/dump.{EXTNAME} python3 cibuild/getsyms.py imports > {PGPATCH}/imports/{EXTNAME}")
                with open(f"{PGPATCH}/imports/{EXTNAME}","r") as f:
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

                if asp.startswith('/dump.'):
                    continue

                fp = PGROOT / asp[1:]
                if fp.is_symlink():
                    continue
                if is_extension(test, fp):
                    #print(f"{EXTNAME=}", test )
                    PACKLIST.append( [fp, test] )
                else:
                    print("custom:", test)


PGROOT=Path(os.environ.get('PGROOT',"/tmp/pglite"))
PGPATCH=Path(os.environ.get('PGPATCH', PGROOT))

INSTALLED = []

EXTNAME = ""
PACKLIST = []
SYMBOLS=[]

PREINST = "/plpgsql"
IS_PREINST = PREINST in sys.argv
for line in open(PGROOT / "pg.installed" ).readlines():
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
        os.chdir(PGROOT)
        with tarfile.open(PGROOT / "sdk" / f"{EXTNAME}.tar" , "w:") as tar:
            for fp, fn in PACKLIST:
                print(f"{EXTNAME} : {fp} => {fn}")
                tar.add(fn.as_posix()[1:])
                #if "builtin" not in sys.argv:
                os.remove(fp)
        os.chdir(swd)
    else:
        print(f"Nothing found to pack for {EXTNAME}, did you 'make install' ?")
else:
        print("Nothing to pack for builtin extension :", EXTNAME)

