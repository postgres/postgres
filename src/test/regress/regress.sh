#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/regress/Attic/regress.sh,v 1.38 2000/01/09 07:53:58 tgl Exp $
#
if [ $# -eq 0 ]
then
	echo "Syntax: $0 <hostname> [extra-tests]"
	exit 1
fi

hostname=$1
shift
extratests="$*"

if [ "x$hostname" = "xwin" -o "x$hostname" = "xqnx4" ]
then
	HOSTLOC="-h localhost"
else
	HOSTLOC=""
fi

if echo '\c' | grep -s c >/dev/null 2>&1
then
	ECHO_N="echo -n"
	ECHO_C=""
else
	ECHO_N="echo"
	ECHO_C='\c'
fi

if [ -d ./obj ]; then
	cd ./obj
fi

PGTZ="PST8PDT"; export PGTZ
PGDATESTYLE="Postgres,US"; export PGDATESTYLE

#FRONTEND=monitor
FRONTEND="psql $HOSTLOC -n -e -q"

echo "=============== Notes...                              ================="
echo "postmaster must already be running for the regression tests to succeed."
echo "Please report any apparent problems to ports@postgresql.org"
echo "See regress/README for more information."
echo ""

echo "=============== dropping old regression database...   ================="
dropdb $HOSTLOC regression

echo "=============== creating new regression database...   ================="
if [ -n "$MULTIBYTE" ];then
	mbtests=`echo $MULTIBYTE | tr "[A-Z]" "[a-z]"`
	PGCLIENTENCODING="$MULTIBYTE"
	export PGCLIENTENCODING
	ENCODINGOPT="-E $MULTIBYTE"
else
	mbtests=""
	unset PGCLIENTENCODING
	ENCODINGOPT=""
fi
createdb $ENCODINGOPT $HOSTLOC regression
if [ $? -ne 0 ]; then
     echo createdb failed
     exit 1
fi

if [ "x$hostname" != "xqnx4" ]
then
echo "=============== installing PL/pgSQL...                ================="
createlang $HOSTLOC plpgsql regression
if [ $? -ne 0 -a $? -ne 2 ]; then
     echo createlang failed
     exit 1
fi
fi

echo "=============== running regression queries...         ================="
echo "" > regression.diffs

if [ "x$hostname" = "xqnx4" ]
then
	DIFFOPT="-b"
else
	DIFFOPT="-w"
fi

stdtests=`awk '
$1=="test"	{ print $2; }
			{}
' < sql/run_check.tests`

for tst in $stdtests $mbtests $extratests
do
	$ECHO_N "${tst} .. " $ECHO_C
	$FRONTEND regression < sql/${tst}.sql > results/${tst}.out 2>&1

	#
	# Check resultmap to see if we should compare to a
	# system-specific result file.  The format of the .similar file is
	#	testname/hostname=substitutefile
	# There shouldn't be multiple matches, but take the last if there are.
	#
	EXPECTED="expected/${tst}.out"
	SUBST=`grep "^$tst/$hostname=" resultmap | sed 's/^.*=//' | tail -1`
	if test "$SUBST"
	then EXPECTED="expected/${SUBST}.out"
	else
	    # Next look for a .similar entry that is a prefix of $hostname.
	    # If there are multiple matches, take the last one.
	    exec 4<resultmap
	    while read LINE <&4
	    do
		SIMHOST=`expr "$LINE" : '\(.*\)='`
		MATCH=`expr "$tst/$hostname" : "$SIMHOST"`
		if test "$MATCH" != 0
		then SUBST=`echo "$LINE" | sed 's/^.*=//'`
		fi
	    done
	    exec 4<&-
	    if test "$SUBST"
	    then EXPECTED="expected/${SUBST}.out"
	    fi
	fi

	if [ `diff ${DIFFOPT} ${EXPECTED} results/${tst}.out | wc -l` -ne 0 ]
	then
		( diff ${DIFFOPT} -C3 ${EXPECTED} results/${tst}.out; \
		echo "";  \
		echo "----------------------"; \
		echo "" ) >> regression.diffs
		echo failed
	else
		echo ok
	fi
done

exit 0

echo "=============== running error queries ...             ================="
$FRONTEND regression < errors.sql
# this will generate error result code

#set this to 1 to avoid clearing the database
debug=0

if test "$debug" -eq 1
then
echo Skipping clearing and deletion of the regression database
else
echo "=============== clearing regression database...       ================="
$FRONTEND regression < drop.sql
if [ $? -ne 0 ]; then
     echo the drop script has an error
     exit 1
fi

exit 0
echo "=============== dropping regression database...       ================="
dropdb regression
if [ $? -ne 0 ]; then
     echo dropdb failed
     exit 1
fi

exit 0
fi
