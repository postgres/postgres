# regress.sh
# rserv regression test script
# (c) 2000 Thomas Lockhart, PostgreSQL Inc.

dropdb master
dropdb slave

createdb master
createdb slave

MasterInit master
SlaveInit slave

psql -c "create table t1 (i int, t text, d timestamp default text 'now');" master
MasterAddTable master t1 d

psql -c "create table t1 (i int, t text, d timestamp default text 'now');" slave
SlaveAddTable slave t1 d

psql -c "insert into t1 values (1, 'one');" master
psql -c "insert into t1 values (2, 'two');" master

Replicate master slave
MasterSync master `GetSyncID --noverbose slave`

psql -c "insert into t1 values (3, 'three');" master
psql -c "insert into t1 values (4, 'four');" master

Replicate master slave
MasterSync master `GetSyncID --noverbose slave`

exit
