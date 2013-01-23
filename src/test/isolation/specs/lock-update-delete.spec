# If we update a tuple, and then delete (or update that touches the key) it,
# and later somebody tries to come along and traverse that update chain,
# he should get an error when locking the latest version, if the delete
# committed; or succeed, when the deleting transaction rolls back.

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
step "s2d"	{ DELETE FROM foo; }
step "s2u2"	{ UPDATE foo SET key = 2 WHERE key = 1; }
step "s2c"	{ COMMIT; }
step "s2r"	{ ROLLBACK; }

permutation "s1b" "s2b" "s1s" "s2u" "s2d" "s1l" "s2c" "s1c"
permutation "s1b" "s2b" "s1s" "s2u" "s2d" "s1l" "s2r" "s1c"
permutation "s1b" "s2b" "s1s" "s2u" "s2u2" "s1l" "s2c" "s1c"
permutation "s1b" "s2b" "s1s" "s2u" "s2u2" "s1l" "s2r" "s1c"
