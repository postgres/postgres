# Test NOWAIT with tuple locks.

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

session "s1"
setup		{ BEGIN; }
step "s1a"	{ SELECT * FROM foo FOR UPDATE; }
step "s1b"	{ COMMIT; }

session "s2"
setup		{ BEGIN; }
step "s2a"	{ SELECT * FROM foo FOR UPDATE; }
step "s2b"	{ COMMIT; }

session "s3"
setup		{ BEGIN; }
step "s3a"	{ SELECT * FROM foo FOR UPDATE NOWAIT; }
step "s3b"	{ COMMIT; }

# s3 skips to second record due to tuple lock held by s2
permutation "s1a" "s2a" "s3a" "s1b" "s2b" "s3b"