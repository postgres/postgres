# MERGE MATCHED RECHECK
#
# This test looks at what happens when we have complex
# WHEN MATCHED AND conditions and a concurrent UPDATE causes a
# recheck of the AND condition on the new row

setup
{
  CREATE TABLE target (key int primary key, balance integer, status text, val text);
  INSERT INTO target VALUES (1, 160, 's1', 'setup');
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
step "merge_status"
{
  MERGE INTO target t
  USING (SELECT 1 as key) s
  ON s.key = t.key
  WHEN MATCHED AND status = 's1' THEN
	UPDATE SET status = 's2', val = t.val || ' when1'
  WHEN MATCHED AND status = 's2' THEN
	UPDATE SET status = 's3', val = t.val || ' when2'
  WHEN MATCHED AND status = 's3' THEN
	UPDATE SET status = 's4', val = t.val || ' when3';
}

step "merge_bal"
{
  MERGE INTO target t
  USING (SELECT 1 as key) s
  ON s.key = t.key
  WHEN MATCHED AND balance < 100 THEN
	UPDATE SET balance = balance * 2, val = t.val || ' when1'
  WHEN MATCHED AND balance < 200 THEN
	UPDATE SET balance = balance * 4, val = t.val || ' when2'
  WHEN MATCHED AND balance < 300 THEN
	UPDATE SET balance = balance * 8, val = t.val || ' when3';
}

step "select1" { SELECT * FROM target; }
step "c1" { COMMIT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "update1" { UPDATE target t SET balance = balance + 10, val = t.val || ' updated by update1' WHERE t.key = 1; }
step "update2" { UPDATE target t SET status = 's2', val = t.val || ' updated by update2' WHERE t.key = 1; }
step "update3" { UPDATE target t SET status = 's3', val = t.val || ' updated by update3' WHERE t.key = 1; }
step "update5" { UPDATE target t SET status = 's5', val = t.val || ' updated by update5' WHERE t.key = 1; }
step "update_bal1" { UPDATE target t SET balance = 50, val = t.val || ' updated by update_bal1' WHERE t.key = 1; }
step "c2" { COMMIT; }

# merge_status sees concurrently updated row and rechecks WHEN conditions, but recheck passes and final status = 's2'
permutation "update1" "merge_status" "c2" "select1" "c1"

# merge_status sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final status = 's3' not 's2'
permutation "update2" "merge_status" "c2" "select1" "c1"

# merge_status sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final status = 's4' not 's2'
permutation "update3" "merge_status" "c2" "select1" "c1"

# merge_status sees concurrently updated row and rechecks WHEN conditions, recheck fails, but we skip update and MERGE does nothing
permutation "update5" "merge_status" "c2" "select1" "c1"

# merge_bal sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final balance = 100 not 640
permutation "update_bal1" "merge_bal" "c2" "select1" "c1"
