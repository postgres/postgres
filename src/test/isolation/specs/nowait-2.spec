# Test NOWAIT with multixact locks.

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
step s1a	{ SELECT * FROM foo FOR SHARE NOWAIT; }
step s1b	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2a	{ SELECT * FROM foo FOR SHARE NOWAIT; }
step s2b	{ SELECT * FROM foo FOR UPDATE NOWAIT; }
step s2c	{ COMMIT; }

# s1 and s2 both get SHARE lock, creating a multixact lock, then s2
# tries to upgrade to UPDATE but aborts because it cannot acquire a
# multi-xact lock
permutation s1a s2a s2b s1b s2c
# the same but with the SHARE locks acquired in a different order, so
# s2 again aborts because it can't acquired a multi-xact lock
permutation s2a s1a s2b s1b s2c
# s2 acquires SHARE then UPDATE, then s1 tries to acquire SHARE but
# can't so aborts because it can't acquire a regular lock
permutation s2a s2b s1a s1b s2c
