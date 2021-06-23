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

session s1
setup		{ BEGIN; }
step s1s	{ SAVEPOINT f; }
step s1u	{ UPDATE foo SET key = 2; }	# obtain KEY REVOKE
step s1r	{ ROLLBACK TO f; } # lose KEY REVOKE
step s1l	{ SELECT * FROM foo FOR KEY SHARE; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2l	{ SELECT * FROM foo FOR KEY SHARE; }
step s2c	{ COMMIT; }

permutation s1s s1u s1r s1l s1c s2l s2c
permutation s1s s1u s1r s1l s2l s1c s2c
permutation s1s s1u s1r s1l s2l s2c s1c
permutation s1s s1u s1r s2l s1l s1c s2c
permutation s1s s1u s1r s2l s1l s2c s1c
permutation s1s s1u s1r s2l s2c s1l s1c
permutation s1s s1u s2l s1r s1l s1c s2c
permutation s1s s1u s2l s1r s1l s2c s1c
permutation s1s s1u s2l s1r s2c s1l s1c
permutation s1s s2l s1u s2c s1r s1l s1c
permutation s1s s2l s2c s1u s1r s1l s1c
permutation s2l s1s s1u s2c s1r s1l s1c
permutation s2l s1s s2c s1u s1r s1l s1c
permutation s2l s2c s1s s1u s1r s1l s1c
