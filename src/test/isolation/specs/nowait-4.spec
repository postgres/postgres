# Test NOWAIT with an updated tuple chain.

setup
{
  CREATE TABLE foo (
	id int PRIMARY KEY,
	data text NOT NULL
  );
  INSERT INTO foo VALUES (1, 'x');
}

teardown
{
  DROP TABLE foo;
}

session s1
setup		{ BEGIN; }
step s1a	{ SELECT * FROM foo WHERE pg_advisory_lock(0) IS NOT NULL FOR UPDATE NOWAIT; }
step s1b	{ COMMIT; }

session s2
step s2a	{ SELECT pg_advisory_lock(0); }
step s2b	{ UPDATE foo SET data = data; }
step s2c	{ BEGIN; }
step s2d	{ UPDATE foo SET data = data; }
step s2e	{ SELECT pg_advisory_unlock(0); }
step s2f	{ COMMIT; }

# s1 takes a snapshot but then waits on an advisory lock, then s2
# updates the row in one transaction, then again in another without
# committing, before allowing s1 to proceed to try to lock a row;
# because it has a snapshot that sees the older version, we reach the
# waiting code in EvalPlanQualFetch which ereports when in NOWAIT mode.
permutation s2a s1a s2b s2c s2d s2e s1b s2f
