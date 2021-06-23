# Test SKIP LOCKED with tuple locks.

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
step s1a	{ SELECT * FROM queue ORDER BY id FOR UPDATE LIMIT 1; }
step s1b	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2a	{ SELECT * FROM queue ORDER BY id FOR UPDATE LIMIT 1; }
step s2b	{ COMMIT; }

session s3
setup		{ BEGIN; }
step s3a	{ SELECT * FROM queue ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1; }
step s3b	{ COMMIT; }

# s3 skips to the second record because it can't obtain the tuple lock
# (s2 holds the tuple lock because it is next in line to obtain the
# row lock, and s1 holds the row lock)
permutation s1a s2a s3a s1b s2b s3b
