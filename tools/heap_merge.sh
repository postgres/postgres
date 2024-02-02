#!/bin/bash

# SCRIPT: patch_generator.sh
#-----------------------------
# This script generates patch between two PG commits and applies it to
# the TDE extension source.

set -o pipefail

## GLOBAL VARIABLES
export TDE="tde"
export SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

export WORKING_DIR="${WORKING_DIR:-$(mktemp -d -t $TDE)}"
export TDE_DIR="${WORKING_DIR}/tde"
export USER_TDE_DIR=""
export PG_COMMIT_BASE="${PG_COMMIT_BASE}"
export PG_COMMIT_LATEST="${PG_COMMIT_BASE}"
export TDE_COMMIT="${TDE_COMMIT}"

export FILES_BASE_DIR="pg_base"
export FILES_LATEST_DIR="pg_latest"
export FILES_PATCH_DIR="pg_patches"
export TDE_DRY_RUN="--dry-run"
export APPLY_PATCHES_FORCE=0

# Script variables
total_patches=0
total_patches_failed=0

declare -a patch_list_unclean=()

declare -a pg_header_file_map=("visibilitymap.h" "rewriteheap.h" "heapam_xlog.h" "hio.h" "heapam.h" "heaptoast.h")
declare -a tde_header_file_map=("pg_tde_visibilitymap.h" "pg_tde_rewrite.h" "pg_tdeam_xlog.h" "pg_tde_io.h" "pg_tdeam.h" "pg_tdetoast.h")

declare -a pg_c_file_map=("heapam.c" "heapam_handler.c" "heapam_visibility.c" "heaptoast.c" "hio.c" "pruneheap.c" "rewriteheap.c" "vacuumlazy.c" "visibilitymap.c")
declare -a tde_c_file_map=("pg_tdeam.c" "pg_tdeam_handler.c" "pg_tdeam_visibility.c" "pg_tdetoast.c" "pg_tde_io.c" "pg_tde_prune.c" "pg_tde_rewrite.c" "pg_tde_vacuumlazy.c" "pg_tde_visibilitymap.c")


## USAGE
usage()
{
    errorCode=${1:-0}

    cat << EOF

usage: $0 OPTIONS

This script generates file-wise patches between two PG commits and applies it to
the TDE extension source.

By default, it only performs a dry run of the patch application. See the usage
options below for applying clean patches or forcefully applying all patches.

It clones both PG and TDE repositories in the working directory. If TDE path is
specified either with its usage option or via the environment variable, then
the script will use the given TDE source code.

* All working folders folders created will carry "$TDE" as part of the folder name.
* This simplies the manual cleanup process.

OPTIONS can be:

  -h  Show this message

  -a                        The patches are not applied by default. Specify this to
                            apply the generated patches. Otherwise, the script will
                            only perform a dryrun.

  -f                        Force apply patches.

  -b  [PG_COMMIT_BASE]      PG base commit hash/branch/tag for patch        [REQUIRED]
  -l  [PG_COMMIT_LATEST]    PG lastest commit hash/branch/tag for patch     [REQUIRED]
  -x  [TDE_COMMIT]          TDE commit hash/branch/tag to apply patch on    [REQUIRED]

  -t  [USER_TDE_DIR]        Source directory for TDE                        [Default: Cloned under WORKING_DIR]
  -w  [WORKING_DIR]         Script working folder                           [Default: $WORKING_DIR]
                            * a folder where patches and relevant log
						      files may be created. This folder will not be removed
                              by the script, so better to keep it in the temp folder.

EOF

    if [[ $errorCode -ne 0 ]];
    then
        exit_script $errorCode
    fi
}

# Perform any required cleanup and exit with the given error/success code
exit_script()
{
    # Reminder of manual cleanup
    if [[ -d $WORKING_DIR ]];
    then
        printf "\n%20s\n" | tr " " "-"
        printf "The following folder was created by the script and may require manual removal.\n"
        printf "* %s\n" $WORKING_DIR
        printf "%20s\n" | tr " " "-"
    fi

    # Exit with a given return code or 0 if none are provided.
    exit ${1:-0}
}

# Raise the error for a failure to checkout required source
checkout_validate()
{
    commit=$1
    retval=$2

    if [[ $rteval -ne 0 ]];
    then
        printf "%s is not a valid commit hash/branch/tag.\n" $commit
        exit_script $retval
    fi
}

# Vaildate arguments to ensure that we can safely run the benchmark
validate_args()
{
    local USAGE_TEXT="See usage for details."
    local PATH_ERROR_TEXT="path is not a valid directory."

    if [[ ! -z "$USER_TDE_DIR" ]];
    then
        if [[ ! -d "$USER_TDE_DIR" ]];
        then
            printf "TDE %s %s\n" $PATH_ERROR_TEXT $USAGE_TEXT >&2
            usage 1
        fi
    elif [[ -z "$TDE_COMMIT" ]];
    then
        printf "TDE_COMMIT is not specified. %s\n" $USAGE_TEXT >&2
        usage 1
    fi


    if [[ ! -d "$WORKING_DIR" ]];
    then
        printf "Working folder %s %s\n" $PATH_ERROR_TEXT $USAGE_TEXT >&2
        usage 1
    fi

    if [[ -z "$PG_COMMIT_BASE" ]];
    then
        printf "PG_COMMIT_BASE is not specified. %s\n" $USAGE_TEXT >&2
        usage 1
    fi

    if [[ -z "$PG_COMMIT_LATEST" ]];
    then
        printf "PG_COMMIT_LATEST is not specified. %s\n" $USAGE_TEXT >&2
        usage 1
    fi
}

# Print the file mapping between PG and TDE
print_map()
{
    printf "\n"
    printf "%50s\n" | tr " " "="
    printf "%s\n" "Heap Access to TDE File Map"
    printf "%50s\n\n" | tr " " "="

    printf "%s\n" "--- Header Files ---"
    for (( i=0; i < ${#pg_header_file_map[@]}; i++ ));
    do
        printf "* %-20s --> %s\n" ${pg_header_file_map[$i]} ${tde_header_file_map[$i]}
    done

    printf "\n"
    printf "%s\n" "--- C Files ---"
    for (( i=0; i < ${#pg_c_file_map[@]}; i++ ));
    do
        printf "* %-20s --> %s\n" ${pg_c_file_map[$i]} ${tde_c_file_map[$i]}
    done

    printf "\n\n"
}

# Copy files from the PG source to the a separate folder. 
# This function expects that we don't have duplicate file names.
copy_files()
{
    local dest_folder=$1
    shift
    local file_list=("$@")
    retval=0

    for f in "${file_list[@]}";
    do
        find * -name $f -exec cp -rpv {} $dest_folder \;
        retval=$?

        if [[ $retval -ne 0 ]];
        then
            exit_script $retval
        fi
    done
}

# Compare two files and generate a patch
generate_file_patch()
{
    f_base=$1
    f_latest=$2
    f_patch=$3

    diff -u $f_base $f_latest > $f_patch

    if [[ ! -s $f_patch ]];
    then
        rm -fv $f_patch
    else
        total_patches=$(expr $total_patches + 1)
    fi
}

# Apply a given patch on a given file
apply_file_patch()
{
    local file_to_patch=$1
    local patch_file=$2
    local apply_patch=${APPLY_PATCHES_FORCE}

    echo "===> $APPLY_PATCHES_FORCE ==> $apply_patch"

    if [[ -f $patch_file ]];
    then
        find * -name $file_to_patch | xargs -I{} echo "patch -p1 -t --dry-run {} $patch_file" | sh

        if [[ $? -ne 0 ]];
        then
            total_patches_failed=$(expr $total_patches_failed + 1)
            patch_list_unclean+=($(basename $patch_file))
            patch_list_unclean+=($(basename $file_to_patch))
        elif [[ -z "$TDE_DRY_RUN" ]];
        then
            apply_patch=1
        fi

        echo "ABOUT TO APPLY PATCH"

        if [[ $apply_patch -eq 1 ]];
        then
        echo "APPLYING PACH"
            find * -name $file_to_patch | xargs -I{} echo "patch -p1 -t {} $patch_file" | sh
        fi        
    fi
}

# Generate file-wise patches using the 
generate_pg_patches()
{
    retval=0

    mkdir $FILES_BASE_DIR
    mkdir $FILES_LATEST_DIR
    mkdir $FILES_PATCH_DIR

    git clone https://github.com/postgres/postgres.git

    # go into the postgres directory
    pushd postgres

    # safety net to ensure that any changes introduced due to git configuration are cleaned up
    git checkout .

    #checkout base source code
    git checkout $PG_COMMIT_BASE
    checkout_validate $PG_COMMIT_BASE $?
    copy_files "$WORKING_DIR/$FILES_BASE_DIR" "${pg_header_file_map[@]}"
    copy_files "$WORKING_DIR/$FILES_BASE_DIR" "${pg_c_file_map[@]}"

    # safety net to ensure that any changes introduced due to git configuration are cleaned up
    git checkout .

    # do the latest checkout
    git checkout $PG_COMMIT_LATEST
    checkout_validate $PG_COMMIT_LATEST $?
    copy_files "$WORKING_DIR/$FILES_LATEST_DIR" "${pg_header_file_map[@]}"
    copy_files "$WORKING_DIR/$FILES_LATEST_DIR" "${pg_c_file_map[@]}"

    # go back to the old directory
    popd

    # generate patches for the header files
    for f in "${pg_header_file_map[@]}";
    do
        generate_file_patch "$FILES_BASE_DIR/$f" "$FILES_LATEST_DIR/$f" "$FILES_PATCH_DIR/$f.patch"
    done

    # generate patches for the c files
    for f in "${pg_c_file_map[@]}";
    do
        generate_file_patch "$FILES_BASE_DIR/$f" "$FILES_LATEST_DIR/$f" "$FILES_PATCH_DIR/$f.patch"
    done
}

# Apply patches to the TDE sources
tde_apply_patches()
{
    # check if the $TDE folder exists. If not, then we have to clone it
    if [[ ! -d "$TDE_DIR" ]];
    then
        t="$(basename $TDE_DIR)"
        git clone https://github.com/Percona-Lab/pg_tde.git $t
    fi

    pushd $TDE_DIR

    # do the required checkout
    git checkout $TDE_COMMIT
    checkout_validate $TDE_COMMIT $?

    # apply patches to the header files
    for (( i=0; i < ${#pg_header_file_map[@]}; i++ ));
    do
        patch_file=$WORKING_DIR/$FILES_PATCH_DIR/${pg_header_file_map[$i]}.patch
        apply_file_patch ${tde_header_file_map[$i]} $patch_file
    done

    # apply patches to the header files
    for (( i=0; i < ${#pg_c_file_map[@]}; i++ ));
    do
        patch_file=$WORKING_DIR/$FILES_PATCH_DIR/${pg_c_file_map[$i]}.patch
        apply_file_patch ${tde_c_file_map[$i]} $patch_file
    done
}

# Check options passed in.
while getopts "haf t:b:l:w:x:" OPTION
do
    case $OPTION in
        h)
            usage
            exit_script 1
            ;;

        a)
            TDE_DRY_RUN=""
            ;;

        f)
            APPLY_PATCHES_FORCE=1
            ;;
        b)
            PG_COMMIT_BASE=$OPTARG
            ;;
        l)
            PG_COMMIT_LATEST=$OPTARG
            ;;
        t)
            TDE_DIR=$OPTARG
            ;;
        w)
            WORK_DIR=$OPTARG
            ;;
        x)
            TDE_COMMIT=$OPTARG
            ;;

        ?)
            usage
            exit_script
            ;;
    esac
done

# Validate and update setup
validate_args

# print the file map
print_map

# Let's move to the working directory
pushd $WORKING_DIR

# generate pg patches between the two commits
generate_pg_patches

# apply patches
tde_apply_patches

# We're done...
printf "\nJob completed!\n"

printf "\n\n"
printf "%50s\n" | tr " " "="
printf "RESULT SUMMARY\n"
printf "%50s\n" | tr " " "="
printf "Patches Generated = %s\n" $total_patches
printf "Patches Applied = %s\n" $(expr $total_patches - $total_patches_failed)
printf "Patches Failed = %s\n" $total_patches_failed

if [[ ${#patch_list_unclean[@]} -gt 0 ]];
then
    printf "=> Failed Patch List\n"
fi

for (( i=0; i < ${#patch_list_unclean[@]}; i++ ));
do
    printf "* %s --> %s\n" ${patch_list_unclean[$i]} ${patch_list_unclean[$(expr $i + 1)]}
    i=$(expr $i + 1)
done

# Perform clean up and exit.
exit_script 0
