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
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.9 1996/10/04 20:07:10 scrappy Exp $
#
#-------------------------------------------------------------------------

# ----------------
#       Set paths from environment or default values.
#       The _fUnKy_..._sTuFf_ gets set when the script is built (with make)
#       from parameters set in the make file.
#       Currently the only thing we look for from the environment is
#       PGDATA, PGHOST, and PGPORT.  However, we should have environment
#       variables for all the paths.  
#
# ----------------
[ -z "$PGDATA" ] && { PGDATA=_fUnKy_DATADIR_sTuFf_; export PGDATA; }
[ -z "$PGPORT" ] && { PGPORT=_fUnKy_POSTPORT_sTuFf_; export PGPORT; }
[ -z "$PGHOST" ] && { PGHOST=localhost; export PGHOST; }
BINDIR=_fUnKy_BINDIR_sTuFf_
LIBDIR=_fUnKy_LIBDIR_sTuFf_
NAMEDATALEN=_fUnKy_NAMEDATALEN_sTuFf_
OIDNAMELEN=_fUnKy_OIDNAMELEN_sTuFf_
PATH=$BINDIR:$PATH
export PATH

CMDNAME=`basename $0`

# Set defaults:
debug=0
noclean=0
template_only=0
POSTGRES_SUPERUSERNAME=$USER

for ARG ; do
# We would normally use e.g. ${ARG#--username=} to parse the options, but
# there is a bug in some shells that makes that not work (BSD4.4 sh,
# September 1996 -- supposed to be fixed in later release).  So we bypass
# the bug with this sed mess.

    username_sed=`echo $ARG | sed s/^--username=//`
    pgdata_sed=`echo $ARG | sed s/^--pgdata=//`

    if [ $ARG = "--debug" -o $ARG = "-d" ]; then
        debug=1
        echo "Running with debug mode on."
    elif [ $ARG = "--noclean" -o $ARG = "-n" ]; then
        noclean=1
        echo "Running with noclean mode on.  Mistakes will not be cleaned up."
    elif [ $ARG = "--template" ]; then
        template_only=1
        echo "updating template1 database only."
    elif [ $username_sed. != $ARG. ]; then
        POSTGRES_SUPERUSERNAME=$username_sed
    elif [ $pgdata_sed. != $ARG. ]; then
        PGDATA=$pgdata_sed
    else    
        echo "Unrecognized option '$ARG'.  Syntax is:"
        echo "initdb [--template] [--debug] [--noclean]" \
             "[--username=SUPERUSER] [--pgdata=DATADIR]"
        exit 100
    fi
done

if [ "$debug" -eq 1 ]; then
    BACKENDARGS="-boot -C -F -d"
else
    BACKENDARGS="-boot -C -F -Q"
fi

TEMPLATE=$LIBDIR/local1_template1.bki.source
GLOBAL=$LIBDIR/global1.bki.source
PG_HBA_SAMPLE=$LIBDIR/pg_hba.sample

#-------------------------------------------------------------------------
# Find the input files
#-------------------------------------------------------------------------

for PREREQ_FILE in $TEMPLATE $GLOBAL $PG_HBA_SAMPLE; do
    if [ ! -f $PREREQ_FILE ]; then 
        echo "$CMDNAME does not find the file '$PREREQ_FILE'."
        echo "This means Postgres95 is incorrectly installed."
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

if [ -z $POSTGRES_SUPERUSERNAME ]; then 
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
    echo "to the specified Postgres userid)."
    exit 2
fi

echo "We are initializing the database system with username" \
  "$POSTGRES_SUPERUSERNAME (uid=$POSTGRES_SUPERUID)."   
echo "Please be aware that Postgres is not secure.  Anyone who can connect"
echo "to the database can act as user $POSTGRES_SUPERUSERNAME" \
     "with very little effort."
echo

# -----------------------------------------------------------------------
# Create the data directory if necessary
# -----------------------------------------------------------------------

# umask must disallow access to group, other for files and dirs
umask 077

if [ -d "$PGDATA" ]; then
    if [ $template_only -eq 0 ]; then
        echo "$CMDNAME: error: Directory $PGDATA already exists."
        echo "This probably means initdb has already been run and the "
        echo "database system already exists."
        echo 
        echo "If you want to create a new database system, either remove "
        echo "the $PGDATA directory or run initdb with environment variable"
        echo "PGDATA set to something other than $PGDATA."
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

    cp $PG_HBA_SAMPLE $PGDATA/pg_hba

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

if [ $debug -eq 0 ]; then
    echo "vacuuming template1"

    echo "vacuum" | postgres -F -Q template1 > /dev/null
fi
