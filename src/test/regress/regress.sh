#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/regress/Attic/regress.sh,v 1.11 1997/06/03 14:19:28 thomas Exp $
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

TZ="PST8PDT"; export TZ

#FRONTEND=monitor
FRONTEND="psql -n -e -q"

echo "=============== Notes...                              ================="
echo "postmaster must already be running for the regression tests to succeed."
echo "The time zone must be set to PST/PDT for the date and time data types"
echo " to pass the regression tests; to do this type"
echo "   setenv TZ $TZ"
echo " before starting postmaster. regress/README has more information."
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
for i in `cat sql/tests`
do
	$ECHO_N "${i} .. " $ECHO_C
	$FRONTEND regression < sql/${i}.sql > results/${i}.out 2>&1
	if [ `diff expected/${i}.out results/${i}.out | wc -l` -ne 0 ]
	then
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
