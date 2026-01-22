# INSERT...ON CONFLICT DO UPDATE test with partitioned table
#
# We use SELECT FOR UPDATE to block the INSERT at the point where
# it has found an existing tuple and is attempting to update it.
# Then we can execute a conflicting update and verify the results.
# Of course, this only works in READ COMMITTED mode, else we'd get an error.

setup
{
  CREATE TABLE upsert (i int PRIMARY KEY, j int, k int) PARTITION BY RANGE (i);
  CREATE TABLE upsert_1 PARTITION OF upsert FOR VALUES FROM (1) TO (100);
  CREATE TABLE upsert_2 PARTITION OF upsert FOR VALUES FROM (100) TO (200);

  INSERT INTO upsert VALUES (1, 10, 100);
}

teardown
{
  DROP TABLE upsert;
}

session s1
setup           { BEGIN ISOLATION LEVEL READ COMMITTED; }
step insert1    { INSERT INTO upsert VALUES (1, 11, 111)
                  ON CONFLICT (i) DO UPDATE SET k = EXCLUDED.k; }
step select1    { SELECT * FROM upsert; }
step c1         { COMMIT; }

session s2
setup           { BEGIN ISOLATION LEVEL READ COMMITTED; }
step lock2      { SELECT * FROM upsert WHERE i = 1 FOR UPDATE; }
step update2a   { UPDATE upsert SET i = i + 10 WHERE i = 1; }
step update2b   { UPDATE upsert SET i = i + 150 WHERE i = 1; }
step delete2    { DELETE FROM upsert WHERE i = 1; }
step c2         { COMMIT; }

# Test case where concurrent update moves the target row within the partition
permutation lock2 insert1 update2a c2 select1 c1
# Test case where concurrent update moves the target row to another partition
permutation lock2 insert1 update2b c2 select1 c1
# Test case where target row is concurrently deleted
permutation lock2 insert1 delete2 c2 select1 c1
