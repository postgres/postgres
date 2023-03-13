# MERGE MATCHED RECHECK
#
# This test looks at what happens when we have complex
# WHEN MATCHED AND conditions and a concurrent UPDATE causes a
# recheck of the AND condition on the new row

setup
{
  CREATE TABLE target (key int primary key, balance integer, status text, val text);
  INSERT INTO target VALUES (1, 160, 's1', 'setup');

  CREATE TABLE target_pa (key int, balance integer, status text, val text) PARTITION BY RANGE (balance);
  CREATE TABLE target_pa1 PARTITION OF target_pa FOR VALUES FROM (0) TO (200);
  CREATE TABLE target_pa2 PARTITION OF target_pa FOR VALUES FROM (200) TO (1000);
  INSERT INTO target_pa VALUES (1, 160, 's1', 'setup');

  CREATE TABLE target_tg (key int primary key, balance integer, status text, val text);
  CREATE FUNCTION target_tg_trig_fn() RETURNS trigger LANGUAGE plpgsql AS
  $$
  BEGIN
    IF tg_op = 'INSERT' THEN
      RAISE NOTICE 'Insert: %', NEW;
      RETURN NEW;
    ELSIF tg_op = 'UPDATE' THEN
      RAISE NOTICE 'Update: % -> %', OLD, NEW;
      RETURN NEW;
    ELSE
      RAISE NOTICE 'Delete: %', OLD;
      RETURN OLD;
    END IF;
  END
  $$;
  CREATE TRIGGER target_tg_trig BEFORE INSERT OR UPDATE OR DELETE ON target_tg
    FOR EACH ROW EXECUTE FUNCTION target_tg_trig_fn();
  INSERT INTO target_tg VALUES (1, 160, 's1', 'setup');
}

teardown
{
  DROP TABLE target;
  DROP TABLE target_pa;
  DROP TABLE target_tg;
  DROP FUNCTION target_tg_trig_fn;
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
step "merge_status_tg"
{
  MERGE INTO target_tg t
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
step "merge_bal_pa"
{
  MERGE INTO target_pa t
  USING (SELECT 1 as key) s
  ON s.key = t.key
  WHEN MATCHED AND balance < 100 THEN
	UPDATE SET balance = balance * 2, val = t.val || ' when1'
  WHEN MATCHED AND balance < 200 THEN
	UPDATE SET balance = balance * 4, val = t.val || ' when2'
  WHEN MATCHED AND balance < 300 THEN
	UPDATE SET balance = balance * 8, val = t.val || ' when3';
}
step "merge_bal_tg"
{
  MERGE INTO target_tg t
  USING (SELECT 1 as key) s
  ON s.key = t.key
  WHEN MATCHED AND balance < 100 THEN
	UPDATE SET balance = balance * 2, val = t.val || ' when1'
  WHEN MATCHED AND balance < 200 THEN
	UPDATE SET balance = balance * 4, val = t.val || ' when2'
  WHEN MATCHED AND balance < 300 THEN
	UPDATE SET balance = balance * 8, val = t.val || ' when3';
}

step "merge_delete"
{
  MERGE INTO target t
  USING (SELECT 1 as key) s
  ON s.key = t.key
  WHEN MATCHED AND balance < 100 THEN
	UPDATE SET balance = balance * 2, val = t.val || ' when1'
  WHEN MATCHED AND balance < 200 THEN
    DELETE;
}
step "merge_delete_tg"
{
  MERGE INTO target_tg t
  USING (SELECT 1 as key) s
  ON s.key = t.key
  WHEN MATCHED AND balance < 100 THEN
	UPDATE SET balance = balance * 2, val = t.val || ' when1'
  WHEN MATCHED AND balance < 200 THEN
    DELETE;
}

step "select1" { SELECT * FROM target; }
step "select1_pa" { SELECT * FROM target_pa; }
step "select1_tg" { SELECT * FROM target_tg; }
step "c1" { COMMIT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "update1" { UPDATE target t SET balance = balance + 10, val = t.val || ' updated by update1' WHERE t.key = 1; }
step "update1_tg" { UPDATE target_tg t SET balance = balance + 10, val = t.val || ' updated by update1_tg' WHERE t.key = 1; }
step "update2" { UPDATE target t SET status = 's2', val = t.val || ' updated by update2' WHERE t.key = 1; }
step "update2_tg" { UPDATE target_tg t SET status = 's2', val = t.val || ' updated by update2_tg' WHERE t.key = 1; }
step "update3" { UPDATE target t SET status = 's3', val = t.val || ' updated by update3' WHERE t.key = 1; }
step "update3_tg" { UPDATE target_tg t SET status = 's3', val = t.val || ' updated by update3_tg' WHERE t.key = 1; }
step "update5" { UPDATE target t SET status = 's5', val = t.val || ' updated by update5' WHERE t.key = 1; }
step "update5_tg" { UPDATE target_tg t SET status = 's5', val = t.val || ' updated by update5_tg' WHERE t.key = 1; }
step "update_bal1" { UPDATE target t SET balance = 50, val = t.val || ' updated by update_bal1' WHERE t.key = 1; }
step "update_bal1_pa" { UPDATE target_pa t SET balance = 50, val = t.val || ' updated by update_bal1_pa' WHERE t.key = 1; }
step "update_bal1_tg" { UPDATE target_tg t SET balance = 50, val = t.val || ' updated by update_bal1_tg' WHERE t.key = 1; }
step "c2" { COMMIT; }

# merge_status sees concurrently updated row and rechecks WHEN conditions, but recheck passes and final status = 's2'
permutation "update1" "merge_status" "c2" "select1" "c1"
permutation "update1_tg" "merge_status_tg" "c2" "select1_tg" "c1"

# merge_status sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final status = 's3' not 's2'
permutation "update2" "merge_status" "c2" "select1" "c1"
permutation "update2_tg" "merge_status_tg" "c2" "select1_tg" "c1"

# merge_status sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final status = 's4' not 's2'
permutation "update3" "merge_status" "c2" "select1" "c1"
permutation "update3_tg" "merge_status_tg" "c2" "select1_tg" "c1"

# merge_status sees concurrently updated row and rechecks WHEN conditions, recheck fails, but we skip update and MERGE does nothing
permutation "update5" "merge_status" "c2" "select1" "c1"
permutation "update5_tg" "merge_status_tg" "c2" "select1_tg" "c1"

# merge_bal sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final balance = 100 not 640
permutation "update_bal1" "merge_bal" "c2" "select1" "c1"
permutation "update_bal1_pa" "merge_bal_pa" "c2" "select1_pa" "c1"
permutation "update_bal1_tg" "merge_bal_tg" "c2" "select1_tg" "c1"

# merge_delete sees concurrently updated row and rechecks WHEN conditions, but recheck passes and row is deleted
permutation "update1" "merge_delete" "c2" "select1" "c1"
permutation "update1_tg" "merge_delete_tg" "c2" "select1_tg" "c1"

# merge_delete sees concurrently updated row and rechecks WHEN conditions, recheck fails, so final balance is 100
permutation "update_bal1" "merge_delete" "c2" "select1" "c1"
permutation "update_bal1_tg" "merge_delete_tg" "c2" "select1_tg" "c1"
