#!/bin/sh
#-------------------------------------------------------------------------
#
# initdb.sh--
#     Create (initialize) a Postgres database system.  
# 
#     A database system is a collection of Postgres databases all managed
#     by the same postmaster.  
#
#     To create the database system, we create the directory that contains
#     all its data, create the files that hold the global classes, create
#     a few other control files for it, and create one database:  the
#     template database.
#
#     The template database is an ordinary Postgres database.  Its data
#     never changes, though.  It exists to make it easy for Postgres to 
#     create other databases -- it just copies.
#
#     Optionally, we can skip creating the database system and just create
#     (or replace) the template database.
#
#     To create all those classes, we run the postgres (backend) program and
#     feed it data from bki files that are in the Postgres library directory.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.25 1997/11/13 03:22:34 momjian Exp $
#
#-------------------------------------------------------------------------

# ----------------
#       The _fUnKy_..._sTuFf_ gets set when the script is built (with make)
#       from parameters set in the make file.
#
# ----------------

NAMEDATALEN=_fUnKy_NAMEDATALEN_sTuFf_
OIDNAMELEN=_fUnKy_OIDNAMELEN_sTuFf_

CMDNAME=`basename $0`

# Find the default PGLIB directory (the directory that contains miscellaneous 
# files that are part of Postgres).  The user-written program postconfig
# outputs variable settings like "PGLIB=/usr/lib/whatever".  If it doesn't
# output a PGLIB value, then there is no default and the user must
# specify the pglib option.  Postconfig may validly not exist, in which case
# our invocation of it silently fails.

# The 2>/dev/null is to swallow the "postconfig: not found" message if there
# is no postconfig.

postconfig_result="`sh -c postconfig 2>/dev/null`"
if [ ! -z "$postconfig_result" ]; then
  set -a   # Make the following variable assignment exported to environment
  eval "$postconfig_result"
  set +a   # back to normal
fi

# Set defaults:
debug=0
noclean=0
template_only=0
POSTGRES_SUPERUSERNAME=$USER

while [ "$#" -gt 0 ]
do
# ${ARG#--username=} is not reliable or available on all platforms

    case "$1" in
        --debug|-d)
                debug=1
                echo "Running with debug mode on."
                ;;
        --noclean|-n)
                noclean=1
                echo "Running with noclean mode on. "
                     "Mistakes will not be cleaned up."
                ;;
        --template|-t)
                template_only=1
                echo "updating template1 database only."
                ;;
        --username=*)
                POSTGRES_SUPERUSERNAME="`echo $1 | sed 's/^--username=//'`"
                ;;
        -u)
                shift
                POSTGRES_SUPERUSERNAME="$1"
                ;;
        -u*)
                POSTGRES_SUPERUSERNAME="`echo $1 | sed 's/^-u//'`"
                ;;
        --pgdata=*)
                PGDATA="`echo $1 | sed 's/^--pgdata=//'`"
                ;;
        -r)
                shift
                PGDATA="$1"
                ;;
        -r*)
                PGDATA="`echo $1 | sed 's/^-r//'`"
                ;;
        --pglib=*)
                PGLIB="`echo $1 | sed 's/^--pglib=//'`"
                ;;
        -l)
                shift
                PGLIB="$1"
                ;;
        -l*)
                PGLIB="`echo $1 | sed 's/^-l//'`"
                ;;

        *)
                echo "Unrecognized option '$1'.  Syntax is:"
                echo "initdb [-t | --template] [-d | --debug]" \
                     "[-n | --noclean]" \
                     "[-u SUPERUSER | --username=SUPERUSER]" \
                     "[-r DATADIR | --pgdata=DATADIR]" \
                     "[-l LIBDIR | --pglib=LIBDIR]"
                exit 100
        esac
        shift
done

#-------------------------------------------------------------------------
# Make sure he told us where to find the Postgres files.
#-------------------------------------------------------------------------
if [ -z "$PGLIB" ]; then
    echo "$CMDNAME does not know where to find the files that make up "
    echo "Postgres (the PGLIB directory).  You must identify the PGLIB "
    echo "directory either with a --pglib invocation option, or by "
    echo "setting the PGLIB environment variable, or by having a program "
    echo "called 'postconfig' in your search path that outputs an asignment "
    echo "for PGLIB."
    exit 20
fi

#-------------------------------------------------------------------------
# Make sure he told us where to build the database system
#-------------------------------------------------------------------------

if [ -z "$PGDATA" ]; then
    echo "$CMDNAME: You must identify the PGDATA directory, where the data"
    echo "for this database system will reside.  Do this with either a"
    echo "--pgdata invocation option or a PGDATA environment variable."
    echo
    exit 20
fi

TEMPLATE=$PGLIB/local1_template1.bki.source
GLOBAL=$PGLIB/global1.bki.source
TEMPLATE_DESCR=$PGLIB/local1_template1.description
GLOBAL_DESCR=$PGLIB/global1.description
PG_HBA_SAMPLE=$PGLIB/pg_hba.conf.sample
PG_GEQO_SAMPLE=$PGLIB/pg_geqo.sample


#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

for PREREQ_FILE in $TEMPLATE $GLOBAL $PG_HBA_SAMPLE; do
    if [ ! -f $PREREQ_FILE ]; then 
        echo "$CMDNAME does not find the file '$PREREQ_FILE'."
        echo "This means you have identified an invalid PGLIB directory."
        echo "You specify a PGLIB directory with a --pglib invocation "
        echo "option, a PGLIB environment variable, or a postconfig program."
        exit 1
    fi
done

echo "$CMDNAME: using $TEMPLATE as input to create the template database."
if [ $template_only -eq 0 ]; then
    echo "$CMDNAME: using $GLOBAL as input to create the global classes."
    echo "$CMDNAME: using $PG_HBA_SAMPLE as the host-based authentication" \
         "control file."
    echo
fi  

#---------------------------------------------------------------------------
# Figure out who the Postgres superuser for the new database system will be.
#---------------------------------------------------------------------------

if [ -z "$POSTGRES_SUPERUSERNAME" ]; then 
    echo "Can't tell what username to use.  You don't have the USER"
    echo "environment variable set to your username and didn't specify the "
    echo "--username option"
    exit 1
fi

POSTGRES_SUPERUID=`pg_id $POSTGRES_SUPERUSERNAME`

if [ $POSTGRES_SUPERUID = NOUSER ]; then
    echo "Valid username not given.  You must specify the username for "
    echo "the Postgres superuser for the database system you are "
    echo "initializing, either with the --username option or by default "
    echo "to the USER environment variable."
    exit 10
fi

if [ $POSTGRES_SUPERUID -ne `pg_id` -a `pg_id` -ne 0 ]; then 
    echo "Only the unix superuser may initialize a database with a different"
    echo "Postgres superuser.  (You must be able to create files that belong"
    echo "to the specified unix user)."
    exit 2
fi

echo "We are initializing the database system with username" \
  "$POSTGRES_SUPERUSERNAME (uid=$POSTGRES_SUPERUID)."   
echo "This user will own all the files and must also own the server process."
echo

# -----------------------------------------------------------------------
# Create the data directory if necessary
# -----------------------------------------------------------------------

# umask must disallow access to group, other for files and dirs
umask 077

if [ -f "$PGDATA/PG_VERSION" ]; then
    if [ $template_only -eq 0 ]; then
        echo "$CMDNAME: error: File $PGDATA/PG_VERSION already exists."
        echo "This probably means initdb has already been run and the "
        echo "database system already exists."
        echo 
        echo "If you want to create a new database system, either remove "
        echo "the $PGDATA directory or run initdb with a --pgdata option "
        echo "other than $PGDATA."
        exit 1
    fi
else
    if [ ! -d $PGDATA ]; then
        echo "Creating Postgres database system directory $PGDATA"
        echo
        mkdir $PGDATA
        if [ $? -ne 0 ]; then exit 5; fi
    fi
    if [ ! -d $PGDATA/base ]; then
        echo "Creating Postgres database system directory $PGDATA/base"
        echo
        mkdir $PGDATA/base
        if [ $? -ne 0 ]; then exit 5; fi
    fi
fi

#----------------------------------------------------------------------------
# Create the template1 database
#----------------------------------------------------------------------------

rm -rf $PGDATA/base/template1
mkdir $PGDATA/base/template1

if [ "$debug" -eq 1 ]; then
    BACKEND_TALK_ARG="-d"
else
    BACKEND_TALK_ARG="-Q"
fi

BACKENDARGS="-boot -C -F -D$PGDATA $BACKEND_TALK_ARG"

echo "$CMDNAME: creating template database in $PGDATA/base/template1"
echo "Running: postgres $BACKENDARGS template1"

cat $TEMPLATE \
| sed -e "s/postgres PGUID/$POSTGRES_SUPERUSERNAME $POSTGRES_SUPERUID/" \
      -e "s/NAMEDATALEN/$NAMEDATALEN/g" \
      -e "s/OIDNAMELEN/$OIDNAMELEN/g" \
      -e "s/PGUID/$POSTGRES_SUPERUID/" \
| postgres $BACKENDARGS template1

if [ $? -ne 0 ]; then
    echo "$CMDNAME: could not create template database"
    if [ $noclean -eq 0 ]; then
        echo "$CMDNAME: cleaning up by wiping out $PGDATA/base/template1"
        rm -rf $PGDATA/base/template1
    else
        echo "$CMDNAME: cleanup not done because noclean options was used."
    fi
    exit 1;
fi

echo

pg_version $PGDATA/base/template1

#----------------------------------------------------------------------------
# Create the global classes, if requested.
#----------------------------------------------------------------------------

if [ $template_only -eq 0 ]; then
    echo "Creating global classes in $PG_DATA/base"
    echo "Running: postgres $BACKENDARGS template1"

    cat $GLOBAL \
    | sed -e "s/postgres PGUID/$POSTGRES_SUPERUSERNAME $POSTGRES_SUPERUID/" \
        -e "s/NAMEDATALEN/$NAMEDATALEN/g" \
        -e "s/OIDNAMELEN/$OIDNAMELEN/g" \
        -e "s/PGUID/$POSTGRES_SUPERUID/" \
    | postgres $BACKENDARGS template1

    if (test $? -ne 0)
    then
        echo "$CMDNAME: could not create global classes."
        if (test $noclean -eq 0); then
            echo "$CMDNAME: cleaning up."
            rm -rf $PGDATA
        else
            echo "$CMDNAME: cleanup not done (noclean mode set)."
        fi
        exit 1;
    fi

    echo

    pg_version $PGDATA

    cp $PG_HBA_SAMPLE $PGDATA/pg_hba.conf
    cp $PG_GEQO_SAMPLE $PGDATA/pg_geqo.sample

    echo "Adding template1 database to pg_database..."

    echo "open pg_database" > /tmp/create.$$
    echo "insert (template1 $POSTGRES_SUPERUID template1)" >> /tmp/create.$$
    #echo "show" >> /tmp/create.$$
    echo "close pg_database" >> /tmp/create.$$

    echo "Running: postgres $BACKENDARGS template1 < /tmp/create.$$"

    postgres $BACKENDARGS template1 < /tmp/create.$$ 

    if [ $? -ne 0 ]; then
        echo "$CMDNAME: could not log template database"
        if [ $noclean -eq 0 ]; then
            echo "$CMDNAME: cleaning up."
            rm -rf $PGDATA
        else
            echo "$CMDNAME: cleanup not done (noclean mode set)."
        fi
        exit 1;
    fi
    rm -f /tmp/create.$$
fi

echo

echo "vacuuming template1"
echo "vacuum" | postgres -F -Q -D$PGDATA template1 2>&1 > /dev/null |\
	grep -v "^DEBUG:"

echo "loading pg_description"
echo "copy pg_description from '$TEMPLATE_DESCR'" | postgres -F -Q -D$PGDATA template1 > /dev/null
echo "copy pg_description from '$GLOBAL_DESCR'" | postgres -F -Q -D$PGDATA template1 > /dev/null

