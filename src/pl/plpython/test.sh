#!/bin/sh

DBNAME=pltest

echo -n "*** Destroy $DBNAME."
dropdb $DBNAME > test.log 2>&1
echo " Done. ***"

echo -n "*** Create $DBNAME."
createdb $DBNAME >> test.log 2>&1
echo " Done. ***"

echo -n "*** Create plpython."
createlang plpythonu $DBNAME >> test.log 2>&1
echo " Done. ***"

echo -n "*** Create tables"
psql -q $DBNAME < plpython_schema.sql >> test.log 2>&1
echo -n ", data"
psql -q $DBNAME < plpython_populate.sql >> test.log 2>&1
echo -n ", and functions and triggers."
psql -q $DBNAME < plpython_function.sql >> test.log 2>&1
echo " Done. ***"

echo -n "*** Running feature tests."
psql -q -e $DBNAME < plpython_test.sql > feature.output 2>&1
echo " Done. ***"

echo -n "*** Running error handling tests."
psql -q -e $DBNAME < plpython_error.sql > error.output 2>&1
echo " Done. ***"

echo -n "*** Checking the results of the feature tests."
if diff -c feature.expected feature.output > feature.diff 2>&1 ; then
  echo -n " passed!"
else
  echo -n " failed!  Please examine feature.diff."
fi
echo " Done. ***"

echo -n "*** Checking the results of the error handling tests."
if diff -c error.expected error.output > error.diff 2>&1 ; then
  echo -n " passed!"
else
  echo -n " failed!  Please examine error.diff."
fi
echo " Done. ***"
