# MERGE NOT MATCHED
#
# Test WHEN NOT MATCHED BY SOURCE/TARGET actions with concurrent updates.

setup
{
  CREATE TABLE src (id int, val text);
  CREATE TABLE tgt (id int, val text);
  INSERT INTO src VALUES (2, 's2');
  INSERT INTO tgt VALUES (1, 't1'), (2, 't2');

  CREATE TABLE p_tgt (id int, val text) PARTITION BY RANGE (id);
  CREATE TABLE p_tgt_p1 PARTITION OF p_tgt FOR VALUES FROM (1) TO (100);
  CREATE TABLE p_tgt_p2 PARTITION OF p_tgt DEFAULT;
  INSERT INTO p_tgt VALUES (1, 't1'), (2, 't2');
}

teardown
{
  DROP TABLE src, tgt, p_tgt;
}

session s1
step b1 { BEGIN ISOLATION LEVEL READ COMMITTED; }
step u1 { UPDATE tgt SET id = 22, val = val||' updated' WHERE id = 2; }
step p_u1 { UPDATE p_tgt SET id = 22, val = val||' updated' WHERE id = 2; }
step c1 { COMMIT; }

session s2
step m2a {
  WITH m AS (
    MERGE INTO tgt t USING src s ON t.id = s.id
      WHEN MATCHED THEN UPDATE SET val = t.val||' matched '||s.val
      WHEN NOT MATCHED BY SOURCE THEN UPDATE SET val = t.val||' not matched'
      RETURNING merge_action(), s.*, t.*
  )
  SELECT * FROM m ORDER BY 4;
}
step p_m2a {
  WITH m AS (
    MERGE INTO p_tgt t USING src s ON t.id = s.id
      WHEN MATCHED THEN UPDATE SET id = t.id+100, val = t.val||' matched '||s.val
      WHEN NOT MATCHED BY SOURCE THEN UPDATE SET id = t.id+100, val = t.val||' not matched'
      RETURNING merge_action(), s.*, t.tableoid::regclass, t.*
  )
  SELECT * FROM m ORDER BY 5;
}
step m2b {
  WITH m AS (
    MERGE INTO tgt t USING src s ON t.id = s.id
      WHEN MATCHED THEN UPDATE SET val = t.val||' matched '||s.val
      WHEN NOT MATCHED BY SOURCE THEN UPDATE SET val = t.val||' not matched'
      WHEN NOT MATCHED BY TARGET THEN INSERT VALUES (s.id, s.val)
      RETURNING merge_action(), s.*, t.*
  )
  SELECT * FROM m ORDER BY 4;
}
step p_m2b {
  WITH m AS (
    MERGE INTO p_tgt t USING src s ON t.id = s.id
      WHEN MATCHED THEN UPDATE SET id = t.id+100, val = t.val||' matched '||s.val
      WHEN NOT MATCHED BY SOURCE THEN UPDATE SET id = t.id+100, val = t.val||' not matched'
      WHEN NOT MATCHED BY TARGET THEN INSERT VALUES (s.id, s.val)
      RETURNING merge_action(), s.*, t.tableoid::regclass, t.*
  )
  SELECT * FROM m ORDER BY 5;
}
step s2 { SELECT * FROM tgt ORDER BY id; }
step p_s2 { SELECT tableoid::regclass, * FROM p_tgt ORDER BY id; }

# Test returning source values under left join
permutation u1 m2a s2
permutation b1 u1 m2a c1 s2

# As above, but merge moves rows to new partition
permutation p_u1 p_m2a p_s2
permutation b1 p_u1 p_m2a c1 p_s2

# Test returning source values under full join
permutation u1 m2b s2
permutation b1 u1 m2b c1 s2

# As above, but merge moves rows to new partition
permutation p_u1 p_m2b p_s2
permutation b1 p_u1 p_m2b c1 p_s2
