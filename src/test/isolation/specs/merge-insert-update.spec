# MERGE INSERT UPDATE
#
# This looks at how we handle concurrent INSERTs, illustrating how the
# behavior differs from INSERT ... ON CONFLICT

setup
{
  CREATE TABLE target (key int primary key, val text);
}

teardown
{
  DROP TABLE target;
}

session "s1"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "merge1" { MERGE INTO target t USING (SELECT 1 as key, 'merge1' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set val = t.val || ' updated by merge1'; }
step "delete1" { DELETE FROM target WHERE key = 1; }
step "insert1" { INSERT INTO target VALUES (1, 'insert1'); }
step "c1" { COMMIT; }
step "a1" { ABORT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "merge2" { MERGE INTO target t USING (SELECT 1 as key, 'merge2' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set val = t.val || ' updated by merge2'; }

step "merge2i" { MERGE INTO target t USING (SELECT 1 as key, 'merge2' as val) s ON s.key = t.key WHEN MATCHED THEN UPDATE set val = t.val || ' updated by merge2'; }

step "select2" { SELECT * FROM target; }
step "c2" { COMMIT; }

# Basic effects
permutation "merge1" "c1" "select2" "c2"
permutation "merge1" "c1" "merge2" "select2" "c2"

# check concurrent inserts
permutation "insert1" "merge2" "c1" "select2" "c2"
permutation "merge1" "merge2" "c1" "select2" "c2"
permutation "merge1" "merge2" "a1" "select2" "c2"

# check how we handle when visible row has been concurrently deleted, then same key re-inserted
permutation "delete1" "insert1" "c1" "merge2" "select2" "c2"
permutation "delete1" "insert1" "merge2" "c1" "select2" "c2"
permutation "delete1" "insert1" "merge2i" "c1" "select2" "c2"
