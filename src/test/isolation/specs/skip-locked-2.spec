# Test SKIP LOCKED with multixact locks.

setup
{
  CREATE TABLE queue (
	id	int		PRIMARY KEY,
	data			text	NOT NULL,
	status			text	NOT NULL
  );
  INSERT INTO queue VALUES (1, 'foo', 'NEW'), (2, 'bar', 'NEW');
}

teardown
{
  DROP TABLE queue;
}

session s1
setup		{ BEGIN; }
step s1a	{ SELECT * FROM queue ORDER BY id FOR SHARE SKIP LOCKED LIMIT 1; }
step s1b	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2a	{ SELECT * FROM queue ORDER BY id FOR SHARE SKIP LOCKED LIMIT 1; }
step s2b	{ SELECT * FROM queue ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1; }
step s2c	{ COMMIT; }

# s1 and s2 both get SHARE lock, creating a multixact lock, then s2
# tries to update to UPDATE but skips the record because it can't
# acquire a multixact lock
permutation s1a s2a s2b s1b s2c

# the same but with the SHARE locks acquired in a different order, so
# s2 again skips because it can't acquired a multixact lock
permutation s2a s1a s2b s1b s2c

# s2 acquires SHARE then UPDATE, then s1 tries to acquire SHARE but
# can't so skips the first record because it can't acquire a regular
# lock
permutation s2a s2b s1a s1b s2c
