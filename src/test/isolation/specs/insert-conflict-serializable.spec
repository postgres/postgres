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
step ioc_select1 { INSERT INTO ioc_a VALUES (1, 99) ON CONFLICT (key) DO SELECT RETURNING val; }
step ioc_select1_keyshare { INSERT INTO ioc_a VALUES (1, 99) ON CONFLICT (key) DO SELECT FOR KEY SHARE RETURNING val; }
step ioc_select1_where { INSERT INTO ioc_a VALUES (1, 99) ON CONFLICT (key) DO SELECT WHERE ioc_a.val > 100 RETURNING val; }
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

# DO SELECT returns the existing row: s2 must fail
permutation ioc_select1 count2 update2 insert1 c1 c2

# DO SELECT FOR KEY SHARE: the non-key update does not conflict with the
# tuple lock, so update2 proceeds without blocking and only the SIREAD
# lock makes s2 fail
permutation ioc_select1_keyshare count2 update2 insert1 c1 c2

# DO SELECT with a WHERE clause rejecting the existing row: the row is not
# returned, but it decided the outcome and was examined by the WHERE
# clause, so it still counts as a read
permutation ioc_select1_where count2 update2 insert1 c1 c2

# If the update is already in flight when DO SELECT runs, the arbiter
# probe waits for it to commit, leaving a conflicting row that is not
# visible to the query snapshot.  The row cannot be returned, so DO
# SELECT must fail instead
permutation count2 update2 ioc_select1 c2 insert1 c1
