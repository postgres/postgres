# Read-write-unique test.

setup
{
  CREATE TABLE test (i integer PRIMARY KEY);
}

teardown
{
  DROP TABLE test;
}

session "s1"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "r1" { SELECT * FROM test WHERE i = 42; }
step "w1" { INSERT INTO test VALUES (42); }
step "c1" { COMMIT; }

session "s2"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "r2" { SELECT * FROM test WHERE i = 42; }
step "w2" { INSERT INTO test VALUES (42); }
step "c2" { COMMIT; }

# Two SSI transactions see that there is no row with value 42
# in the table, then try to insert that value; T1 inserts,
# and then T2 blocks waiting for T1 to commit.  Finally,
# T2 reports a serialization failure.

permutation "r1" "r2" "w1" "w2" "c1" "c2"

# If the value is already visible before T2 begins, then a
# regular unique constraint violation should still be raised
# by T2.

permutation "r1" "w1" "c1" "r2" "w2" "c2"
