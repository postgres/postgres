#!/bin/sh

dropdb -U postgres dblink_test_master
createdb -U postgres dblink_test_master
psql -U postgres dblink_test_master < `pwd`/dblink.sql

dropdb -U postgres dblink_test_slave
createdb -U postgres dblink_test_slave
psql -U postgres dblink_test_slave < `pwd`/dblink.sql

psql -eaq -U postgres template1 < `pwd`/dblink.test.sql > dblink.test.out 2>&1
diff -c ./dblink.test.expected.out `pwd`/dblink.test.out > dblink.test.diff
ls -l dblink.test.diff


