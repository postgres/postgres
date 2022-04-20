# MERGE DELETE
#
# This test looks at the interactions involving concurrent deletes
# comparing the behavior of MERGE, DELETE and UPDATE

setup
{
  CREATE TABLE target (key int primary key, val text);
  INSERT INTO target VALUES (1, 'setup1');
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
step "delete" { DELETE FROM target t WHERE t.key = 1; }
step "merge_delete" { MERGE INTO target t USING (SELECT 1 as key) s ON s.key = t.key WHEN MATCHED THEN DELETE; }
step "c1" { COMMIT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "update1" { UPDATE target t SET val = t.val || ' updated by update1' WHERE t.key = 1; }
step "merge2" { MERGE INTO target t USING (SELECT 1 as key, 'merge2a' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val; }
step "select2" { SELECT * FROM target; }
step "c2" { COMMIT; }

# Basic effects
permutation "delete" "c1" "select2" "c2"
permutation "merge_delete" "c1" "select2" "c2"

# One after the other, no concurrency
permutation "delete" "c1" "update1" "select2" "c2"
permutation "merge_delete" "c1" "update1" "select2" "c2"
permutation "delete" "c1" "merge2" "select2" "c2"
permutation "merge_delete" "c1" "merge2" "select2" "c2"

# Now with concurrency
permutation "delete" "update1" "c1" "select2" "c2"
permutation "merge_delete" "update1" "c1" "select2" "c2"
permutation "delete" "merge2" "c1" "select2" "c2"
permutation "merge_delete" "merge2" "c1" "select2" "c2"
