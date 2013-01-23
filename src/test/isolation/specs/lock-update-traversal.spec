# When a tuple that has been updated is locked, the locking command
# should traverse the update chain; thus, a DELETE should not be able
# to proceed until the lock has been released.

setup
{
  CREATE TABLE foo (
	key		int PRIMARY KEY,
	value	int
  );

  INSERT INTO foo VALUES (1, 1);
}

teardown
{
  DROP TABLE foo;
}

session "s1"
step "s1b"	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "s1s"	{ SELECT * FROM foo; }	# obtain snapshot
step "s1l"	{ SELECT * FROM foo FOR KEY SHARE; } # obtain lock
step "s1c"	{ COMMIT; }

session "s2"
step "s2b"	{ BEGIN; }
step "s2u"	{ UPDATE foo SET value = 2 WHERE key = 1; }
step "s2c"	{ COMMIT; }
step "s2d"	{ DELETE FROM foo WHERE key = 1; }

permutation "s1b" "s2b" "s1s" "s2u" "s1l" "s2c" "s2d" "s1c"
