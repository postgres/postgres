#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/regress/Attic/regress.sh,v 1.1.1.1 1996/07/09 06:22:24 scrappy Exp $
#
if [ -d ./obj ]; then
	cd ./obj
fi

#FRONTEND=monitor
FRONTEND="psql -n -e -q"

echo =============== destroying old regression database... =================
destroydb regression

echo =============== creating new regression database... =================
createdb regression
if [ $? -ne 0 ]; then
     echo createdb failed
     exit 1
fi

$FRONTEND regression < create.sql
if [ $? -ne 0 ]; then
     echo the creation script has an error
     exit 1
fi

echo =============== running regression queries ... =================
$FRONTEND regression < queries.sql
if [ $? -ne 0 ]; then
     echo the queries script causes an error
     exit 1
fi

echo =============== running error queries ... =================
$FRONTEND regression < errors.sql
if [ $? -ne 0 ]; then
     echo the errors script has an unanticipated problem
     exit 1
fi

#set this to 1 to avoid clearing the database
debug=0

if test "$debug" -eq 1
then
echo Skipping clearing and deletion of the regression database
else
echo =============== clearing regression database... =================
$FRONTEND regression < destroy.sql
if [ $? -ne 0 ]; then
     echo the destroy script has an error
     exit 1
fi

exit 0
echo =============== destroying regression database... =================
destroydb regression
if [ $? -ne 0 ]; then
     echo destroydb failed
     exit 1
fi

exit 0
fi
