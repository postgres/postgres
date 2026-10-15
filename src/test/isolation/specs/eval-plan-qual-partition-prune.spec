# Test run-time partition pruning after an EPQ recheck

setup
{
  CREATE TABLE lp (a int PRIMARY KEY, b int) PARTITION BY LIST (a);
  CREATE TABLE lp1 PARTITION OF lp FOR VALUES IN (1);
  CREATE TABLE lp2 PARTITION OF lp FOR VALUES IN (2);
  INSERT INTO lp VALUES (1, 1), (2, 1);
  CREATE TABLE lk (id int PRIMARY KEY, n int);
  INSERT INTO lk VALUES (1, 0);
  CREATE TABLE lt (id int PRIMARY KEY, k int, v int, done bool DEFAULT false);
  INSERT INTO lt VALUES (1, 1, 1);
}

teardown
{
  DROP TABLE lp, lk, lt;
}

session s1
step s1_begin	{ BEGIN; }
step s1_update	{ UPDATE lk SET n = n + 1 WHERE id = 1; }
step s1_update_lt	{ UPDATE lt SET k = 2, v = 2 WHERE id = 1; }
step s1_commit	{ COMMIT; }

session s2
setup
{
  SET enable_hashjoin = off;
  SET enable_mergejoin = off;
  SET enable_seqscan = off;
}
step s2_update
{
  WITH locked AS (
    SELECT * FROM lk WHERE id = 1 FOR UPDATE
  ), upd AS (
    UPDATE lp SET b = lp.b + 1 + (SELECT count(*) FROM locked)
    FROM lp lp2 WHERE lp2.a = lp.a
    RETURNING lp.*
  )
  SELECT * FROM upd ORDER BY a;
}
# The correlated subquery's exec pruning must use the EPQ's param values
step s2_update_lt
{
  UPDATE lt SET done = true
  WHERE lt.v = (SELECT lp.a FROM lp WHERE lp.a = lt.k)
  RETURNING *;
}
step s2_select	{ SELECT * FROM lp ORDER BY a; }

permutation s1_begin s1_update s2_update s1_commit s2_select
permutation s1_begin s1_update_lt s2_update_lt s1_commit
