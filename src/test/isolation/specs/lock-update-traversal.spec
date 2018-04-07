# When a tuple that has been updated is locked, the locking command must
# traverse the update chain; thus, a DELETE (on the newer version of the tuple)
# should not be able to proceed until the lock has been released.  An UPDATE
# that changes the key should not be allowed to continue either; but an UPDATE
# that doesn't modify the key should be able to continue immediately.

setup
{
  CREATE TABLE foo (
	key		int,
	value	int,
	PRIMARY KEY (key) INCLUDE (value)
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
step "s2d1"	{ DELETE FROM foo WHERE key = 1; }
step "s2d2"	{ UPDATE foo SET key = 3 WHERE key = 1; }
step "s2d3"	{ UPDATE foo SET value = 3 WHERE key = 1; }

permutation "s1b" "s2b" "s1s" "s2u" "s1l" "s2c" "s2d1" "s1c"
permutation "s1b" "s2b" "s1s" "s2u" "s1l" "s2c" "s2d2" "s1c"
permutation "s1b" "s2b" "s1s" "s2u" "s1l" "s2c" "s2d3" "s1c"
