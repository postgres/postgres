# Test addition of a partition with less-than-exclusive locking.

setup
{
  CREATE TABLE foo (a int, b text) PARTITION BY LIST(a);
  CREATE TABLE foo1 PARTITION OF foo FOR VALUES IN (1);
  CREATE TABLE foo3 PARTITION OF foo FOR VALUES IN (3);
  CREATE TABLE foo4 PARTITION OF foo FOR VALUES IN (4);
  INSERT INTO foo VALUES (1, 'ABC');
  INSERT INTO foo VALUES (3, 'DEF');
  INSERT INTO foo VALUES (4, 'GHI');
}

teardown
{
  DROP TABLE foo;
}

# The SELECT will be planned with just the three partitions shown above,
# of which we expect foo1 to be pruned at planning and foo3 at execution.
# Then we'll block, and by the time the query is actually executed,
# partition foo2 will also exist.  We expect that not to be scanned.
# This test is specifically designed to check ExecCreatePartitionPruneState's
# code for matching up the partition lists in such cases.

session "s1"
step "s1exec"	{ LOAD 'delay_execution';
		  SET delay_execution.post_planning_lock_id = 12345;
		  SELECT * FROM foo WHERE a <> 1 AND a <> (SELECT 3); }

session "s2"
step "s2lock"	{ SELECT pg_advisory_lock(12345); }
step "s2unlock"	{ SELECT pg_advisory_unlock(12345); }
step "s2addp"	{ CREATE TABLE foo2 (LIKE foo);
		  ALTER TABLE foo ATTACH PARTITION foo2 FOR VALUES IN (2);
		  INSERT INTO foo VALUES (2, 'ADD2'); }

permutation "s2lock" "s1exec" "s2addp" "s2unlock"
