#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/regress/Attic/regress.sh,v 1.37 2000/01/06 06:40:18 thomas Exp $
#
if [ $# -eq 0 ]
then
	echo "Syntax: $0 <portname> [extra-tests]"
	exit 1
fi

portname=$1
shift
extratests="$*"

if [ x$portname = "xwin" -o x$portname = "xqnx4" ]
then
	HOST="-h localhost"
else
	HOST=""
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
FRONTEND="psql $HOST -n -e -q"

SYSTEM=`../../config.guess | awk -F\- '{ split($3,a,/[0-9]/); printf"%s-%s", $1, a[1] }'`

echo "=============== Notes...                              ================="
echo "postmaster must already be running for the regression tests to succeed."
echo "The time zone is set to PST8PDT for these tests by the client frontend."
echo "Please report any apparent problems to ports@postgresql.org"
echo "See regress/README for more information."
echo ""

echo "=============== dropping old regression database...   ================="
dropdb $HOST regression

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
createdb $ENCODINGOPT $HOST regression
if [ $? -ne 0 ]; then
     echo createdb failed
     exit 1
fi

if [ x$portname != "xqnx4" ]
then
echo "=============== installing PL/pgSQL...                ================="
createlang $HOST plpgsql regression
if [ $? -ne 0 -a $? -ne 2 ]; then
     echo createlang failed
     exit 1
fi
fi

echo "=============== running regression queries...         ================="
echo "" > regression.diffs

if [ x$portname = "xqnx4" ]
then
	DIFFOPT="-b"
else
	DIFFOPT="-w"
fi

stdtests=`awk '
$1=="test"	{ print $2; }
			{}
' < sql/run_check.tests`

for i in $stdtests $mbtests $extratests
do
	$ECHO_N "${i} .. " $ECHO_C
	$FRONTEND regression < sql/${i}.sql > results/${i}.out 2>&1
	if [ -f expected/${i}-${SYSTEM}.out ]
	then
		EXPECTED="expected/${i}-${SYSTEM}.out"
	else
		EXPECTED="expected/${i}.out"
	fi
  
	if [ `diff ${DIFFOPT} ${EXPECTED} results/${i}.out | wc -l` -ne 0 ]
	then
		( diff ${DIFFOPT} -C3 ${EXPECTED} results/${i}.out; \
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
