#!/bin/sh

DBNAME=pltest
DBUSER=postgres
PATH=$PATH:/usr/local/pgsql/bin
export DBNAME DBUSER

echo -n "*** Destroy $DBNAME."
dropdb -U$DBUSER $DBNAME > test.log 2>&1
echo " Done. ***"

echo -n "*** Create $DBNAME."
createdb -U$DBUSER $DBNAME >> test.log 2>&1
echo " Done. ***"

echo -n "*** Create plpython."
psql -U$DBUSER -q $DBNAME < plpython_create.sql >> test.log 2>&1
echo " Done. ***"

echo -n "*** Create tables"
psql -U$DBUSER -q $DBNAME < plpython_schema.sql >> test.log 2>&1
echo -n ", data"
psql -U$DBUSER -q $DBNAME < plpython_populate.sql >> test.log 2>&1
echo -n ", and functions and triggers."
psql -U$DBUSER -q $DBNAME < plpython_function.sql >> test.log 2>&1
echo " Done. ***"

echo -n "*** Running feature tests."
psql -U$DBUSER -q -e $DBNAME < plpython_test.sql > feature.output 2>&1
echo " Done. ***"

echo -n "*** Running error handling tests."
psql -U$DBUSER -q -e $DBNAME < plpython_error.sql > error.output 2>&1
echo " Done. ***"

echo -n "*** Checking the results of the feature tests"
if diff -u feature.expected feature.output > feature.diff 2>&1 ; then
  echo -n " passed!"
else
  echo -n " failed!  Please examine feature.diff."
fi
echo " Done. ***"

echo -n "*** Checking the results of the error handling tests."
diff -u error.expected error.output > error.diff 2>&1
echo " Done. ***"
echo "*** You need to check the file error.diff and make sure that"
echo "    any differences are due only to the oid encoded in the "
echo "    python function name. ***"

# or write a fancier error checker...

