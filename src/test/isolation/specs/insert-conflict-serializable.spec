# INSERT ... ON CONFLICT at SERIALIZABLE
#
# The conflicting row decides the outcome of the statement, so it counts
# as a read for SSI purposes, whether or not the statement then writes
# anything: a concurrent transaction writing that row must create a
# rw-antidependency.  These permutations build the classic write-skew
# cycle: s1 reads a and writes b, while s2 reads b and writes a.  One of
# the two transactions must fail with a serialization error.

setup
{
  CREATE TABLE ioc_a (key int PRIMARY KEY, val int);
  CREATE TABLE ioc_b (key int PRIMARY KEY, val int);
  INSERT INTO ioc_a VALUES (1, 0);
}

teardown
{
  DROP TABLE ioc_a, ioc_b;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
}
step ioc_nothing1 { INSERT INTO ioc_a VALUES (1, 99) ON CONFLICT (key) DO NOTHING; }
step ioc_update1_where { INSERT INTO ioc_a VALUES (1, 99) ON CONFLICT (key) DO UPDATE SET val = 99 WHERE ioc_a.val > 100; }
step insert1 { INSERT INTO ioc_b VALUES (1, 10); }
step c1 { COMMIT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
}
step count2 { SELECT count(*) FROM ioc_b; }
step update2 { UPDATE ioc_a SET val = 1 WHERE key = 1; }
step delete2 { DELETE FROM ioc_a WHERE key = 1; }
step c2 { COMMIT; }

# DO NOTHING skips the insert because of the existing row, which s2 then
# deletes or updates: s2 must fail to commit
permutation ioc_nothing1 count2 delete2 insert1 c1 c2
permutation ioc_nothing1 count2 update2 insert1 c1 c2

# DO UPDATE with a WHERE clause rejecting the existing row writes nothing,
# but the row still decided the outcome: s2 must fail
permutation ioc_update1_where count2 update2 insert1 c1 c2
