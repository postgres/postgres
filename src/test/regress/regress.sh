#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/regress/Attic/regress.sh,v 1.18 1998/03/15 07:39:04 scrappy Exp $
#
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
FRONTEND="psql -n -e -q"

SYSTEM=`uname -s`

echo "=============== Notes...                              ================="
echo "postmaster must already be running for the regression tests to succeed."
echo "The time zone is now set to PST8PDT explicitly by this regression test"
echo " client frontend. Please report any apparent problems to"
echo "   ports@postgresql.org"
echo "See regress/README for more information."
echo ""

echo "=============== destroying old regression database... ================="
destroydb regression

echo "=============== creating new regression database...   ================="
createdb regression
if [ $? -ne 0 ]; then
     echo createdb failed
     exit 1
fi

echo "=============== running regression queries...         ================="
echo "" > regression.diffs
if [ a$MB != a ];then
	mbtests=`echo $MB|tr A-Z a-z`
else
	mbtests=""
fi
for i in `cat sql/tests` $mbtests
do
	$ECHO_N "${i} .. " $ECHO_C
	$FRONTEND regression < sql/${i}.sql > results/${i}.out 2>&1
	if [ -f expected/${i}-${SYSTEM}.out ]
	then
		EXPECTED="expected/${i}-${SYSTEM}.out"
	else
		EXPECTED="expected/${i}.out"
	fi
  
	if [ `diff ${EXPECTED} results/${i}.out | wc -l` -ne 0 ]
	then
		( diff -wC3 ${EXPECTED} results/${i}.out; \
		echo "";  \
		echo "----------------------"; \
		echo "" ) >> regression.diffs
		echo failed
	else
		echo ok
	fi
done
exit

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
$FRONTEND regression < destroy.sql
if [ $? -ne 0 ]; then
     echo the destroy script has an error
     exit 1
fi

exit 0
echo "=============== destroying regression database...     ================="
destroydb regression
if [ $? -ne 0 ]; then
     echo destroydb failed
     exit 1
fi

exit 0
fi
