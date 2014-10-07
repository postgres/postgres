# Test SKIP LOCKED when regular row locks can't be acquired.

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

session "s1"
setup		{ BEGIN; }
step "s1a"	{ SELECT * FROM queue ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1; }
step "s1b"	{ SELECT * FROM queue ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1; }
step "s1c"	{ COMMIT; }

session "s2"
setup		{ BEGIN; }
step "s2a"	{ SELECT * FROM queue ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1; }
step "s2b"	{ SELECT * FROM queue ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1; }
step "s2c"	{ COMMIT; }
