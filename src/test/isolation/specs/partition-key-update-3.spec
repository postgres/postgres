# Concurrent update of a partition key and INSERT...ON CONFLICT DO NOTHING
# test on partitioned table with multiple rows in higher isolation levels.
#
# Note: This test is resemble to insert-conflict-do-nothing-2 test

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
step s2beginrr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2begins	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2donothing { INSERT INTO foo VALUES(1, 'session-2 donothing') ON CONFLICT DO NOTHING; }
step s2c { COMMIT; }
step s2select { SELECT * FROM foo ORDER BY a; }

session s3
step s3beginrr { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s3begins { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s3donothing { INSERT INTO foo VALUES(2, 'session-3 donothing'), (2, 'session-3 donothing2') ON CONFLICT DO NOTHING; }
step s3c { COMMIT; }

permutation s2beginrr s3beginrr s1u s2donothing s1c s2c s3donothing s3c s2select
permutation s2beginrr s3beginrr s1u s3donothing s1c s3c s2donothing s2c s2select
permutation s2beginrr s3beginrr s1u s2donothing s3donothing s1c s2c s3c s2select
permutation s2beginrr s3beginrr s1u s3donothing s2donothing s1c s3c s2c s2select
permutation s2begins s3begins s1u s2donothing s1c s2c s3donothing s3c s2select
permutation s2begins s3begins s1u s3donothing s1c s3c s2donothing s2c s2select
permutation s2begins s3begins s1u s2donothing s3donothing s1c s2c s3c s2select
permutation s2begins s3begins s1u s3donothing s2donothing s1c s3c s2c s2select
