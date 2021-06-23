# If we already hold a lock of a given strength, do not deadlock when
# some other transaction is waiting for a conflicting lock and we try
# to acquire the same lock we already held.
setup
{
  CREATE TABLE justthis (
	value	int
  );

  INSERT INTO justthis VALUES (1);
}

teardown
{
  DROP TABLE justthis;
}

session s1
setup			{ BEGIN; }
step s1lock		{ SELECT * FROM justthis FOR SHARE; }
step s1svpt		{ SAVEPOINT foo; }
step s1lock2	{ SELECT * FROM justthis FOR SHARE; }
step s1c		{ COMMIT; }

session s2
setup			{ BEGIN; }
step s2lock		{ SELECT * FROM justthis FOR SHARE; }	# ensure it's a multi
step s2c		{ COMMIT; }

session s3
setup			{ BEGIN; }
step s3lock		{ SELECT * FROM justthis FOR UPDATE; }
step s3c		{ COMMIT; }

permutation s1lock s2lock s1svpt s3lock s1lock2 s2c s1c s3c
