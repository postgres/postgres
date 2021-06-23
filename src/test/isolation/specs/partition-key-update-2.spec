# Concurrent update of a partition key and INSERT...ON CONFLICT DO NOTHING test
#
# This test tries to expose problems with the interaction between concurrent
# sessions during an update of the partition key and INSERT...ON CONFLICT DO
# NOTHING on a partitioned table.
#
# The convention here is that session 1 moves row from one partition to
# another due update of the partition key and session 2 always ends up
# inserting, and session 3 always ends up doing nothing.
#
# Note: This test is slightly resemble to insert-conflict-do-nothing test.

setup
{
  CREATE TABLE foo (a int primary key, b text) PARTITION BY LIST(a);
  CREATE TABLE foo1 PARTITION OF foo FOR VALUES IN (1);
  CREATE TABLE foo2 PARTITION OF foo FOR VALUES IN (2);
  INSERT INTO foo VALUES (1, 'initial tuple');
}

teardown
{
  DROP TABLE foo;
}

session s1
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1u	{ UPDATE foo SET a=2, b=b || ' -> moved by session-1' WHERE a=1; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2donothing { INSERT INTO foo VALUES(1, 'session-2 donothing') ON CONFLICT DO NOTHING; }
step s2c	{ COMMIT; }

session s3
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s3donothing { INSERT INTO foo VALUES(2, 'session-3 donothing') ON CONFLICT DO NOTHING; }
step s3select { SELECT * FROM foo ORDER BY a; }
step s3c	{ COMMIT; }

# Regular case where one session block-waits on another to determine if it
# should proceed with an insert or do nothing.
permutation s1u s2donothing s3donothing s1c s2c s3select s3c
permutation s2donothing s1u s3donothing s1c s2c s3select s3c
