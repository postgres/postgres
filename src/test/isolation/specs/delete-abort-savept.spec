# After rolling back a subtransaction that upgraded a lock, the previously
# held lock should still be held.
setup
{
  CREATE TABLE foo (
     key INT PRIMARY KEY,
     value INT
  );

  INSERT INTO foo VALUES (1, 1);
}

teardown
{
  DROP TABLE foo;
}

session "s1"
setup			{ BEGIN; }
step "s1l"		{ SELECT * FROM foo FOR KEY SHARE; }
step "s1svp"	{ SAVEPOINT f; }
step "s1d"		{ DELETE FROM foo; }
step "s1r"		{ ROLLBACK TO f; }
step "s1c"		{ COMMIT; }

session "s2"
setup			{ BEGIN; }
step "s2l"		{ SELECT * FROM foo FOR UPDATE; }
step "s2c"		{ COMMIT; }
