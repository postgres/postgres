# MERGE DELETE
#
# This test looks at the interactions involving concurrent deletes
# comparing the behavior of MERGE, DELETE and UPDATE

setup
{
  CREATE TABLE target (key int primary key, val text);
  INSERT INTO target VALUES (1, 'setup1');

  CREATE TABLE target_pa (key int primary key, val text) PARTITION BY LIST (key);
  CREATE TABLE target_pa1 PARTITION OF target_pa FOR VALUES IN (1);
  CREATE TABLE target_pa2 PARTITION OF target_pa FOR VALUES IN (2);
  INSERT INTO target_pa VALUES (1, 'setup1');

  CREATE TABLE target_tg (key int primary key, val text);
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
  INSERT INTO target_tg VALUES (1, 'setup1');
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
step "delete" { DELETE FROM target t WHERE t.key = 1; }
step "delete_pa" { DELETE FROM target_pa t WHERE t.key = 1; }
step "delete_tg" { DELETE FROM target_tg t WHERE t.key = 1; }
step "c1" { COMMIT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "update2" { UPDATE target t SET val = t.val || ' updated by update2' WHERE t.key = 1; }
step "update2_pa" { UPDATE target_pa t SET val = t.val || ' updated by update2_pa' WHERE t.key = 1; }
step "update2_tg" { UPDATE target_tg t SET val = t.val || ' updated by update2_tg' WHERE t.key = 1; }
step "merge2" { MERGE INTO target t USING (SELECT 1 as key, 'merge2' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val; }
step "merge2_pa" { MERGE INTO target_pa t USING (SELECT 1 as key, 'merge2_pa' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val; }
step "merge2_tg" { MERGE INTO target_tg t USING (SELECT 1 as key, 'merge2_tg' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val; }
step "merge_delete2" { MERGE INTO target t USING (SELECT 1 as key, 'merge_delete2' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN DELETE; }
step "merge_delete2_tg" { MERGE INTO target_tg t USING (SELECT 1 as key, 'merge_delete2_tg' as val) s ON s.key = t.key WHEN NOT MATCHED THEN INSERT VALUES (s.key, s.val) WHEN MATCHED THEN DELETE; }
step "select2" { SELECT * FROM target; }
step "select2_pa" { SELECT * FROM target_pa; }
step "select2_tg" { SELECT * FROM target_tg; }
step "c2" { COMMIT; }

# Basic effects
permutation "delete" "c1" "select2" "c2"
permutation "delete_pa" "c1" "select2_pa" "c2"
permutation "delete_tg" "c1" "select2_tg" "c2"

# One after the other, no concurrency
permutation "delete" "c1" "update2" "select2" "c2"
permutation "delete_pa" "c1" "update2_pa" "select2_pa" "c2"
permutation "delete_tg" "c1" "update2_tg" "select2_tg" "c2"
permutation "delete" "c1" "merge2" "select2" "c2"
permutation "delete_pa" "c1" "merge2_pa" "select2_pa" "c2"
permutation "delete_tg" "c1" "merge2_tg" "select2_tg" "c2"
permutation "delete" "c1" "merge_delete2" "select2" "c2"
permutation "delete_tg" "c1" "merge_delete2_tg" "select2_tg" "c2"

# Now with concurrency
permutation "delete" "update2" "c1" "select2" "c2"
permutation "delete_pa" "update2_pa" "c1" "select2_pa" "c2"
permutation "delete_tg" "update2_tg" "c1" "select2_tg" "c2"
permutation "delete" "merge2" "c1" "select2" "c2"
permutation "delete_pa" "merge2_pa" "c1" "select2_pa" "c2"
permutation "delete_tg" "merge2_tg" "c1" "select2_tg" "c2"
permutation "delete" "merge_delete2" "c1" "select2" "c2"
permutation "delete_tg" "merge_delete2_tg" "c1" "select2_tg" "c2"
