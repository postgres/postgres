# Test predicate locks on HOT updated tuples.
#
# This test has two serializable transactions. Both select two rows
# from the table, and then update one of them.
# If these were serialized (run one at a time), the transaction that
# runs later would see one of the rows to be updated.
#
# Any overlap between the transactions must cause a serialization failure.
# We used to have a bug in predicate locking HOT updated tuples, which
# caused the conflict to be missed, if the row was HOT updated.

setup
{
  CREATE TABLE test (i int PRIMARY KEY, t text);
  INSERT INTO test VALUES (5, 'apple'), (7, 'pear'), (11, 'banana');
  -- HOT-update 'pear' row.
  UPDATE test SET t = 'pear_hot_updated' WHERE i = 7;
}

teardown
{
  DROP TABLE test;
}

session s1
step b1 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r1 { SELECT * FROM test WHERE i IN (5, 7) }
step w1 { UPDATE test SET t = 'pear_xact1' WHERE i = 7 }
step c1 { COMMIT; }

session s2
step b2 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r2 { SELECT * FROM test WHERE i IN (5, 7) }
step w2 { UPDATE test SET t = 'apple_xact2' WHERE i = 5 }
step c2 { COMMIT; }

permutation b1 b2 r1 r2 w1 w2 c1 c2
