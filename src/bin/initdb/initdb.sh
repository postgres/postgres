#!/bin/sh
#-------------------------------------------------------------------------
#
# initdb.sh--
#    create a postgres template database
#
#    this program feeds the proper input to the ``postgres'' program
#    to create a postgres database and register it in the
#    shared ``pg_database'' database.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/initdb/Attic/initdb.sh,v 1.4 1996/07/23 03:03:19 scrappy Exp $
#
#-------------------------------------------------------------------------

# ----------------
#       Set paths from environment or default values.
#       The _fUnKy_..._sTuFf_ gets set when the script is installed
#       from the default value for this build.
#       Currently the only thing wee look for from the environment is
#       PGDATA, PGHOST, and PGPORT
#
# ----------------
[ -z "$PGDATA" ] && { PGDATA=_fUnKy_DATADIR_sTuFf_; export PGDATA; }
[ -z "$PGPORT" ] && { PGPORT=5432; export PGPORT; }
[ -z "$PGHOST" ] && { PGHOST=localhost; export PGHOST; }
POSTGRESDIR=_fUnKy_POSTGRESDIR_sTuFf_
BINDIR=_fUnKy_BINDIR_sTuFf_
FILESDIR=$PGDATA/files
PATH=$BINDIR:$PATH
export PATH

CMDNAME=`basename $0`

# ----------------
# 	check arguments:
# 	    -d indicates debug mode.
#	    -n means don't clean up on error (so your cores don't go away)
# ----------------
debug=0
noclean=0
verbose=0

for ARG
do
	case "$ARG" in
	-d)	debug=1; echo "$CMDNAME: debug mode on";;
	-n)	noclean=1; echo "$CMDNAME: noclean mode on";;
	-v)	verbose=1; echo "$CMDNAME: verbose mode on";;
	*)	echo "initdb [-d][-n][-v]\n -d : debug mode\n -n : noclean mode, leaves temp files around \n -v : verbose mode";  exit 0;
	esac
done

# ----------------
# 	if the debug flag is set, then 
# ----------------
if test "$debug" -eq 1
then
    BACKENDARGS="-boot -C -F -d"
else
    BACKENDARGS="-boot -C -F -Q"
fi


TEMPLATE=$FILESDIR/local1_template1.bki
GLOBAL=$FILESDIR/global1.bki
if [ ! -f $TEMPLATE -o ! -f $GLOBAL ]
then
    echo "$CMDNAME: error: database initialization files not found."
    echo "$CMDNAME: either gmake install has not been run or you're trying to"
    echo "$CMDNAME: run this program on a machine that does not store the"
    echo "$CMDNAME: database (PGHOST doesn't work for this)."
    exit 1
fi

if test "$verbose" -eq 1
then
    echo "$CMDNAME: using $TEMPLATE"
    echo "$CMDNAME: using $GLOBAL"
fi

#
# Figure out who I am...
#

PG_UID=`pg_id`

if test $PG_UID -eq 0
then
    echo "$CMDNAME: do not install POSTGRES as root"
    exit 1
fi

# ----------------
# 	create the template database if necessary
#	the first we do is create data/base, so we'll check for that.
# ----------------

if test -d "$PGDATA/base"
then
	echo "$CMDNAME: error: it looks like initdb has already been run.  You must"
	echo "clean out the database directory first with the cleardbdir program"
	exit 1
fi

# umask must disallow access to group, other for files and dirs
umask 077

mkdir $PGDATA/base $PGDATA/base/template1

if test "$verbose" -eq 1
then
    echo "$CMDNAME: creating SHARED relations in $PGDATA"
    echo "$CMDNAME: creating template database in $PGDATA/base/template1"
    echo "postgres $BACKENDARGS template1 < $TEMPLATE "
fi

postgres $BACKENDARGS template1 < $TEMPLATE 


if test $? -ne 0
then
    echo "$CMDNAME: could not create template database"
    if test $noclean -eq 0
    then
	    echo "$CMDNAME: cleaning up."
	    cd $PGDATA
	    for i in *
	    do
		if [ $i != "files" -a $i != "pg_hba" ]
		then
			/bin/rm -rf $i
		fi
	    done
        else
	    echo "$CMDNAME: cleanup not done (noclean mode set)."
    fi
	exit 1;
fi

pg_version $PGDATA/base/template1

#
# Add the template database to pg_database
#

echo "open pg_database" > /tmp/create.$$
echo "insert (template1 $PG_UID template1)" >> /tmp/create.$$
#echo "show" >> /tmp/create.$$
echo "close pg_database" >> /tmp/create.$$

if test "$verbose" -eq 1
then
	echo "postgres $BACKENDARGS template1 < $GLOBAL"
fi

postgres $BACKENDARGS template1 < $GLOBAL 

if (test $? -ne 0)
then
    echo "$CMDNAME: could create shared relations"
    if (test $noclean -eq 0)
    then
	    echo "$CMDNAME: cleaning up."
	    cd $PGDATA
	    for i in *
	    do
		if [ $i != "files" ]
		then
			/bin/rm -rf $i
		fi
	    done
    else
	    echo "$CMDNAME: cleanup not done (noclean mode set)."
    fi
	exit 1;
fi

pg_version $PGDATA

if test "$verbose" -eq 1
then
	echo "postgres $BACKENDARGS template1 < /tmp/create.$$"
fi

postgres $BACKENDARGS template1 < /tmp/create.$$ 

if test $? -ne 0
then
    echo "$CMDNAME: could not log template database"
    if (test $noclean -eq 0)
    then
	    echo "$CMDNAME: cleaning up."
	    cd $PGDATA
	    for i in *
	    do
		if [ $i != "files" ]
		then
			/bin/rm -rf $i
		fi
	    done
    else
	    echo "$CMDNAME: cleanup not done (noclean mode set)."
    fi
	exit 1;
fi

if test $debug -eq 0
then

if test "$verbose" -eq 1
then
    echo "vacuuming template1"
fi

    echo "vacuum" | postgres -F -Q template1 > /dev/null
fi

rm -f /tmp/create.$$
