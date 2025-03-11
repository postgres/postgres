#!/usr/bin/env python3

import sys
import os


def dbg(*argv, **kw):
    kw.setdefault("file", sys.stderr)
    return print(*argv, **kw)


SNIFF = ""


# we use output from wasi `wasm-objdump -x` run

exports = []
recording = False

SECTION = sys.argv[-1].lower()

if SECTION.startswith("export"):
    BEGIN = "Export["
    END = "Start:"
    SECTION = "exports"
    WAY = "-> "

    with open(os.environ.get("PGDUMP", "dump.postgres"), "r") as pgdump:
        for line in pgdump.readlines():
            line = line.strip(";\n\r")
            if line:
                print(f"_{line}")

else:
    BEGIN = "Import["
    END = "Function["
    SECTION = "imports"
    WAY = "<- "

with open(os.environ.get("OBJDUMP", "dump.wasm-objdump"), "r") as wasmdump:
    for line in wasmdump.readlines():
        line = line.strip()

        if not line:
            continue
        if SNIFF:
            if line.find(SNIFF) >= 0:
                dbg(line, recording)

        if recording:
            if line.startswith(END):
                dbg("-" * 40)
                recording = False
                break

            exports.append(line)
            if line[0] != "-":
                dbg(line)

        else:
            if line[0] == "-":
                continue

            if line.startswith(BEGIN):
                dbg("\n\n#", line)
                recording = True
                continue
            dbg("skip", line)

dbg(f"found {len(exports)} {SECTION}")

if 1:
    badchars = '"<> '
    VERBOSE = "-v" in sys.argv
    REPORT = []
    for line in exports:
        typ, header = line.split("] ", 1)
        if typ.startswith("- func["):
            typ = "def"
        elif typ.startswith("- global["):
            pass
            typ = "var"
        elif typ.startswith("- memory["):
            pass
            typ = "mem"
        elif typ.startswith("- table["):
            pass
            typ = "tbl"

        if SNIFF:
            if line.find(SNIFF) >= 0:
                dbg(
                    f"""

-------------------------------------------------------------------------------------

{line=}
{typ=}


-------------------------------------------------------------------------------------

"""
                )

        try:
            if typ in ("def", "var"):
                left, right = header.rsplit(WAY, 1)
                left = left.strip(badchars)
                right = right.strip(badchars)
                # GOT.mem. GOT.func. env.
                clean = False
                for clean in (".", " <"):
                    if left.find(clean) >= 0:
                        _, left = left.rsplit(clean, 1)
                        _, right = right.rsplit(".", 1)
                        clean = True
                        break
                if clean:
                    left = left.strip(badchars)
                if right.find(".") > 0:
                    _, right = right.rsplit(".", 1)
                right = right.strip(badchars)

                if left.find("=") > 0:
                    left = right

                if left.find("=") > 0:
                    left = ""

                if not left:
                    left = right

                if SNIFF:
                    if line.find(SNIFF) >= 0:
                        dbg(f"{left=} {right=} '{line}'")

                if left.find("=") > 0:
                    left = ""
                elif left.find("::") > 0:
                    if VERBOSE:
                        raise Exception("bad export (c++)")
                    # continue
                elif left.find(" ") > 0:
                    if VERBOSE:
                        raise Exception("bad export (space)")
                    continue

                if VERBOSE:
                    demangle = os.popen(f"c++filt {right}").read().strip()
                    if not left or demangle == left:
                        dbg(typ, right, "# right")
                    elif demangle != right:
                        dbg(typ, left, "#", demangle)
                    else:
                        dbg(typ, "LEFT=", left, "RIGHT=", demangle)

                if (right or left) not in REPORT:
                    REPORT.append(right or left)
        except Exception as e:
            dbg("ERROR", typ, header, e)

    dbg(len(REPORT), "unique")
    NODUNDER = []

    for rep in REPORT:
        if rep in NODUNDER:
            print(f"{rep}")
        else:
            print(f"_{rep}")
