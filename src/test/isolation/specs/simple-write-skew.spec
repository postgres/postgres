# Write skew test.
#
# This test has two serializable transactions: one which updates all
# 'apple' rows to 'pear' and one which updates all 'pear' rows to
# 'apple'.  If these were serialized (run one at a time) either
# value could be present, but not both.  One must be rolled back to
# prevent the write skew anomaly.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
  CREATE TABLE test (i int PRIMARY KEY, t text);
  INSERT INTO test VALUES (5, 'apple'), (7, 'pear'), (11, 'banana');
}

teardown
{
  DROP TABLE test;
}

session s1
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rwx1 { UPDATE test SET t = 'apple' WHERE t = 'pear'; }
step c1 { COMMIT; }

session s2
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step rwx2 { UPDATE test SET t = 'pear' WHERE t = 'apple'}
step c2 { COMMIT; }
