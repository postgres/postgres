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

permutation "s1l" "s1svp" "s1d" "s1r" "s1c" "s2l" "s2c"
permutation "s1l" "s1svp" "s1d" "s1r" "s2l" "s1c" "s2c"
permutation "s1l" "s1svp" "s1d" "s2l" "s1r" "s1c" "s2c"
permutation "s1l" "s1svp" "s2l" "s1d" "s1r" "s1c" "s2c"
permutation "s1l" "s2l" "s1svp" "s1d" "s1r" "s1c" "s2c"
permutation "s2l" "s1l" "s2c" "s1svp" "s1d" "s1r" "s1c"
permutation "s2l" "s2c" "s1l" "s1svp" "s1d" "s1r" "s1c"
