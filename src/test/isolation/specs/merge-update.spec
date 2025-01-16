# MERGE UPDATE
#
# This test exercises atypical cases
# 1. UPDATEs of PKs that change the join in the ON clause
# 2. UPDATEs with WHEN conditions that would fail after concurrent update
# 3. UPDATEs with extra ON conditions that would fail after concurrent update
# 4. UPDATEs with duplicate source rows

setup
{
  CREATE TABLE target (key int primary key, val text);
  INSERT INTO target VALUES (1, 'setup1');

  CREATE TABLE pa_target (key integer, val text)
	  PARTITION BY LIST (key);
  CREATE TABLE part1 (key integer, val text);
  CREATE TABLE part2 (val text, key integer);
  CREATE TABLE part3 (key integer, val text);

  ALTER TABLE pa_target ATTACH PARTITION part1 FOR VALUES IN (1,4);
  ALTER TABLE pa_target ATTACH PARTITION part2 FOR VALUES IN (2,5,6);
  ALTER TABLE pa_target ATTACH PARTITION part3 DEFAULT;

  INSERT INTO pa_target VALUES (1, 'initial');
  INSERT INTO pa_target VALUES (2, 'initial');
}

teardown
{
  DROP TABLE target;
  DROP TABLE pa_target CASCADE;
}

session "s1"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "merge1"
{
  MERGE INTO target t
  USING (SELECT 1 as key, 'merge1' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
    UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val;
}
step "pa_merge1"
{
  MERGE INTO pa_target t
  USING (SELECT 1 as key, 'pa_merge1' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
    UPDATE set val = t.val || ' updated by ' || s.val;
}
step "pa_merge2"
{
  MERGE INTO pa_target t
  USING (SELECT 1 as key, 'pa_merge2' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
    UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val;
}
step "pa_merge3"
{
  MERGE INTO pa_target t
  USING (SELECT 1 as key, 'pa_merge2' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
    UPDATE set val = 'prefix ' || t.val;
}
step "c1" { COMMIT; }
step "a1" { ABORT; }

session "s2"
setup
{
  BEGIN ISOLATION LEVEL READ COMMITTED;
}
step "merge2a"
{
  MERGE INTO target t
  USING (SELECT 1 as key, 'merge2a' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
	UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val
  WHEN NOT MATCHED BY SOURCE THEN
	UPDATE set key = t.key + 1, val = t.val || ' source not matched by merge2a'
  RETURNING merge_action(), old, new, t.*;
}
step "merge2b"
{
  MERGE INTO target t
  USING (SELECT 1 as key, 'merge2b' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED AND t.key < 2 THEN
	UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val;
}
step "merge2c"
{
  MERGE INTO target t
  USING (SELECT 1 as key, 'merge2c' as val) s
  ON s.key = t.key AND t.key < 2
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
	UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val;
}
step "pa_merge2a"
{
  MERGE INTO pa_target t
  USING (SELECT 1 as key, 'pa_merge2a' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED THEN
	UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val
  WHEN NOT MATCHED BY SOURCE THEN
	UPDATE set key = t.key + 1, val = t.val || ' source not matched by pa_merge2a'
  RETURNING merge_action(), old, new, t.*;
}
# MERGE proceeds only if 'val' unchanged
step "pa_merge2b_when"
{
  MERGE INTO pa_target t
  USING (SELECT 1 as key, 'pa_merge2b_when' as val) s
  ON s.key = t.key
  WHEN NOT MATCHED THEN
	INSERT VALUES (s.key, s.val)
  WHEN MATCHED AND t.val like 'initial%' THEN
	UPDATE set key = t.key + 1, val = t.val || ' updated by ' || s.val;
}
# Duplicate source row should fail
step "pa_merge2c_dup"
{
  MERGE INTO pa_target t
  USING (VALUES (1), (1)) v(a)
  ON t.key = v.a
  WHEN MATCHED THEN
	UPDATE set val = t.val || ' updated by pa_merge2c_dup';  -- should fail
}
step "select2" { SELECT * FROM target; }
step "pa_select2" { SELECT * FROM pa_target; }
step "c2" { COMMIT; }
step "a2" { ABORT; }

# Basic effects
permutation "merge1" "c1" "select2" "c2"

# One after the other, no concurrency
permutation "merge1" "c1" "merge2a" "select2" "c2"
permutation "pa_merge1" "c1" "pa_merge2c_dup" "a2"

# Now with concurrency
permutation "merge1" "merge2a" "c1" "select2" "c2"
permutation "merge1" "merge2a" "a1" "select2" "c2"
permutation "merge1" "merge2b" "c1" "select2" "c2"
permutation "merge1" "merge2c" "c1" "select2" "c2"
permutation "pa_merge1" "pa_merge2a" "c1" "pa_select2" "c2"
permutation "pa_merge2" "pa_merge2a" "c1" "pa_select2" "c2" # fails
permutation "pa_merge2" "c1" "pa_merge2a" "pa_select2" "c2" # succeeds
permutation "pa_merge3" "pa_merge2b_when" "c1" "pa_select2" "c2" # WHEN not satisfied by updated tuple
permutation "pa_merge1" "pa_merge2b_when" "c1" "pa_select2" "c2" # WHEN satisfied by updated tuple
permutation "pa_merge1" "pa_merge2c_dup" "c1" "a2"
