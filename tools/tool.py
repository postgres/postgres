# Simple helper script for upstream merges to the copied heap code
# It implements a few simple steps which can be used to automate
# most operations
#
# Generally this script assumes that pg_tde is checked out as a 
# submodule inside postgres, in the contrib/pg_tde directory. 
#
# Most methods interact with the currently checked out version
# of postgres, this part is not automated at all. Select the
# correct commit before executing functions!
#
# == copy <dst_folder>
#
# Copies the required heapam source files from the postgres repo,
# to the specified <dst_folder> inside the pg_tde repo. Also
# renames the files, places them in the correct directory, and
# runs the automatic sed replacement script.
#
# The sed replacements only cover the name changes, mainly changing "heap"
# to "tdeheap". It doesn't apply the actual encryption changes!
#
# It also creates a file named "COMMIT" in the directory, which contains the
# commit hash used.
#
# == diff <folder1> <folder2> <diff_folder>
#
# Runs diff on the tdeheap files between <folder1> and  <folder2>, and places
# the results into <diff_folder>
#
# The assumption is that <folder1> contains the copied, but not TDEfied
# version of the files, while <folder2> is the actual current TDEfied code,
# and that way this command creates the "tde patch" for the given commit.
# 
# For example, assuming that we have the PG16 tde sources in the src16
# directory, these steps create a diff for the current sources:
# 1. check out the src16/COMMIT commit
# 2. run `copy tmp_16dir`
# 3. run `diff tmp_16dir src16 diff16`
# 4. delete the tmp_16dir directory
# 
# == apply <target_folder> <diff_folder>
# 
# Applies the diffs created by the diff command from the <diff_folder> to the
# <target_folder> source directory.
# 
# When the diff can't be applied cleanly, and there are conflicts, it still
# writes the file with conflicts, using the diff3 format (usual git conflict
# markers). which can be resolved manually.
# 
# The recommended action in this case is to first create a commit with the
# conflicts as-is, and then create a separate commit with the conflicts 
# resolved and the code working.
# 
# This is mainly intended for version upgrades.
# For example, if the current version is 16, and the goal is creating the 17
# version:
# 1. create the src16 diff using the steps described in the `diff` section
# 2. checkout the 17 version in the postgres repo
# 3. use the copy command to create a base directory for the 17 version
# 4. create a commit with the src17 basefiles
# 5. use the apply command to apply the patches
# 6. commit things with conflicts
# 7. resolve the conflicts as needed
# 8. commit resolved/working sources


import shutil
import os
import subprocess
import sys

tools_directory = os.path.dirname(os.path.realpath(__file__))

pg_root = tools_directory + "/../../../"
heapam_src_dir = pg_root + "src/backend/access/heap/"
heapam_inc_dir = pg_root + "src/include/access/"

tde_root = tools_directory + "/../"

heapam_headers = {
    "visibilitymap.h": "pg_tde_visibilitymap.h",
    "rewriteheap.h": "pg_tde_rewrite.h",
    "heapam_xlog.h": "pg_tdeam_xlog.h",
    "hio.h": "pg_tde_io.h",
    "heapam.h": "pg_tdeam.h",
    "heaptoast.h": "pg_tdetoast.h"
}

heapam_sources = {
    "heapam.c": "pg_tdeam.c",
    "heapam_handler.c": "pg_tdeam_handler.c",
    "heapam_visibility.c": "pg_tdeam_visibility.c",
    "heaptoast.c": "pg_tdetoast.c",
    "hio.c": "pg_tde_io.c",
    "pruneheap.c": "pg_tde_prune.c",
    "rewriteheap.c": "pg_tde_rewrite.c",
    "vacuumlazy.c": "pg_tde_vacuumlazy.c",
    "visibilitymap.c": "pg_tde_visibilitymap.c",
}

def copy_and_sed_things(files, src, dst):
    os.makedirs(dst, exist_ok=True)
    for original,copy in files.items():
        print(" - ", original, "=>", copy)
        shutil.copyfile(src+original, dst+copy)
        subprocess.call(["sed", "-i", "-f", tools_directory + "/repl.sed", dst+copy])

def copy_upstream_things(dstdir):
    print("Processing headers")
    copy_and_sed_things(heapam_headers, heapam_inc_dir, tde_root + dstdir + "/include/access/")
    print("Processing sources")
    copy_and_sed_things(heapam_sources, heapam_src_dir, tde_root + dstdir + "/access/")
    # Also create a commit file
    cwd = os.getcwd()
    os.chdir(pg_root)
    commit_hash = subprocess.check_output(["git", "rev-parse", "HEAD"])
    os.chdir(cwd)
    f = open(tde_root + dstdir + "/COMMIT", "w")
    f.write(commit_hash.decode("utf-8"))
    f.close()


def save_diffs(files, src, dst, diffdir):
    os.makedirs(tde_root + "/" + diffdir, exist_ok=True)
    for _,copy in files.items():
        print(" - ", copy + ".patch")
        diff = subprocess.run(["diff", "-u", tde_root+src+"/"+copy, tde_root+dst+"/"+copy], stdout = subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        f = open(tde_root + "/" + diffdir + "/" + copy + ".patch", "w")
        f.write(diff.stdout.decode("utf-8"))
        f.close()

def diff_things(src, dst, diffdir):
    print("Processing headers")
    save_diffs(heapam_headers, src + "/include/access/", dst + "/include/access/", diffdir)
    print("Processing sources")
    save_diffs(heapam_sources, src + "/access/", dst + "/access/", diffdir)

def apply_diffs(files, dst, diffdir):
    for _,copy in files.items():
        print(" - ", copy + ".patch")
        patch = subprocess.run(["patch", "--merge=diff3", "-l", "--no-backup-if-mismatch", tde_root+dst+"/"+copy, tde_root+"/"+diffdir+"/"+copy+".patch"], stdout = subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        print(patch.stdout.decode("utf-8"))
        print(patch.stderr.decode("utf-8"))

def apply_things(dst, diffdir):
    print("Processing headers")
    apply_diffs(heapam_headers, dst + "/include/access/", diffdir)
    print("Processing sources")
    apply_diffs(heapam_sources, dst + "/access/", diffdir)

def rm_files(files, src):
    for _,copy in files.items():
        print(" - RM ", copy)
        os.remove(tde_root+src+"/"+copy)

def rm_things(srcdir):
    print("Processing headers")
    rm_files(heapam_headers, srcdir + "/include/access/")
    print("Processing sources")
    rm_files(heapam_sources, srcdir + "/access/")

if len(sys.argv) < 2:
    print("No command given! Commands:")
    print(" - copy")
    print(" - diff")
    print(" - ppply")
    print(" - rm    ")
    exit()

if sys.argv[1] == "copy":
    if len(sys.argv) < 3:
        print("No target directory given!")
        print("Usage: tool.py copy <dstdir>")
        exit()
    copy_upstream_things(sys.argv[2])

if sys.argv[1] == "diff":
    if len(sys.argv) < 5:
        print("Not enough parameters!")
        print("Usage: tool.py diff <copied_dir> <current_dir> <diff_dir>")
        exit()
    diff_things(sys.argv[2], sys.argv[3], sys.argv[4])

if sys.argv[1] == "apply":
    if len(sys.argv) < 4:
        print("Not enough parameters!")
        print("Usage: tool.py patch <src_dir> <diff_dir>")
        exit()
    apply_things(sys.argv[2], sys.argv[3])



if sys.argv[1] == "rm":
    if len(sys.argv) < 3:
        print("No target directory given!")
        print("Usage: tool.py rm <dstdir>")
        exit()
    rm_things(sys.argv[2])