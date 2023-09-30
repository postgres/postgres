# MERGE JOIN
#
# This test checks the EPQ recheck mechanism during MERGE when joining to a
# source table using different join methods, per bug #18103

setup
{
  CREATE TABLE src (id int PRIMARY KEY, val int);
  CREATE TABLE tgt (id int PRIMARY KEY, val int);
  INSERT INTO src SELECT x, x*10 FROM generate_series(1,3) g(x);
  INSERT INTO tgt SELECT x, x FROM generate_series(1,3) g(x);
}

teardown
{
  DROP TABLE src, tgt;
}

session s1
step b1  { BEGIN ISOLATION LEVEL READ COMMITTED; }
step m1  { MERGE INTO tgt USING src ON tgt.id = src.id
             WHEN MATCHED THEN UPDATE SET val = src.val
             WHEN NOT MATCHED THEN INSERT VALUES (src.id, src.val); }
step s1  { SELECT * FROM tgt; }
step c1  { COMMIT; }

session s2
step b2  { BEGIN ISOLATION LEVEL READ COMMITTED; }
step hj  { SET LOCAL enable_mergejoin = off; SET LOCAL enable_nestloop = off; }
step mj  { SET LOCAL enable_hashjoin = off; SET LOCAL enable_nestloop = off; }
step nl  { SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off; }
step ex  { EXPLAIN (verbose, costs off)
           MERGE INTO tgt USING src ON tgt.id = src.id
             WHEN MATCHED THEN UPDATE SET val = src.val
             WHEN NOT MATCHED THEN INSERT VALUES (src.id, src.val); }
step m2  { MERGE INTO tgt USING src ON tgt.id = src.id
             WHEN MATCHED THEN UPDATE SET val = src.val
             WHEN NOT MATCHED THEN INSERT VALUES (src.id, src.val); }
step s2  { SELECT * FROM tgt; }
step c2  { COMMIT; }

permutation b1 m1 s1 c1 b2 m2 s2 c2
permutation b1 b2 m1 hj ex m2 c1 c2 s1
permutation b1 b2 m1 mj ex m2 c1 c2 s1
permutation b1 b2 m1 nl ex m2 c1 c2 s1
