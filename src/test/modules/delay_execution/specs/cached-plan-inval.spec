# Test to check that invalidation of cached generic plans during ExecutorStart
# is correctly detected causing an updated plan to be re-executed.

setup
{
  CREATE TABLE foo (a int, b text) PARTITION BY RANGE (a);
  CREATE TABLE foo1 PARTITION OF foo FOR VALUES FROM (MINVALUE) TO (3) PARTITION BY RANGE (a);
  CREATE TABLE foo1_1 PARTITION OF foo1 FOR VALUES FROM (MINVALUE) TO (2);
  CREATE TABLE foo1_2 PARTITION OF foo1 FOR VALUES FROM (2) TO (3);
  CREATE INDEX foo1_1_a ON foo1_1 (a);
  CREATE TABLE foo2 PARTITION OF foo FOR VALUES FROM (3) TO (MAXVALUE);
  INSERT INTO foo SELECT generate_series(-1000, 1000);
  CREATE VIEW foov AS SELECT * FROM foo;
  CREATE FUNCTION one () RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE PLPGSQL STABLE;
  CREATE FUNCTION two () RETURNS int AS $$ BEGIN RETURN 2; END; $$ LANGUAGE PLPGSQL STABLE;
  CREATE TABLE bar (a int, b text) PARTITION BY LIST(a);
  CREATE TABLE bar1 PARTITION OF bar FOR VALUES IN (1);
  CREATE INDEX ON bar1(a);
  CREATE TABLE bar2 PARTITION OF bar FOR VALUES IN (2);
  CREATE RULE update_foo AS ON UPDATE TO foo DO ALSO UPDATE bar SET a = a WHERE a = one();
  CREATE RULE update_bar AS ON UPDATE TO bar DO ALSO SELECT 1;
  ANALYZE;
}

teardown
{
  DROP VIEW foov;
  DROP RULE update_foo ON foo;
  DROP TABLE foo, bar;
  DROP FUNCTION one(), two();
}

session "s1"
step "s1prep"   { SET plan_cache_mode = force_generic_plan;
		  PREPARE q AS SELECT * FROM foov WHERE a = $1 FOR UPDATE;
		  EXPLAIN (COSTS OFF) EXECUTE q (1); }

step "s1prep2"   { SET plan_cache_mode = force_generic_plan;
		  PREPARE q2 AS SELECT * FROM foov WHERE a = one() or a = two();
		  EXPLAIN (COSTS OFF) EXECUTE q2; }

step "s1prep3"   { SET plan_cache_mode = force_generic_plan;
		  PREPARE q3 AS UPDATE foov SET a = a WHERE a = one() or a = two();
		  EXPLAIN (COSTS OFF) EXECUTE q3; }

step "s1prep4"   { SET plan_cache_mode = force_generic_plan;
		  PREPARE q4 AS SELECT * FROM generate_series(1, 1) WHERE EXISTS (SELECT * FROM foov WHERE a = $1 FOR UPDATE);
		  EXPLAIN (COSTS OFF) EXECUTE q4 (1); }

step "s1exec"	{ LOAD 'delay_execution';
		  SET delay_execution.executor_start_lock_id = 12345;
		  EXPLAIN (COSTS OFF) EXECUTE q (1); }
step "s1exec2"	{ LOAD 'delay_execution';
		  SET delay_execution.executor_start_lock_id = 12345;
		  EXPLAIN (COSTS OFF) EXECUTE q2; }
step "s1exec3"	{ LOAD 'delay_execution';
		  SET delay_execution.executor_start_lock_id = 12345;
		  EXPLAIN (COSTS OFF) EXECUTE q3; }
step "s1exec4"	{ LOAD 'delay_execution';
		  SET delay_execution.executor_start_lock_id = 12345;
		  EXPLAIN (COSTS OFF) EXECUTE q4 (1); }

session "s2"
step "s2lock"	{ SELECT pg_advisory_lock(12345); }
step "s2unlock"	{ SELECT pg_advisory_unlock(12345); }
step "s2dropi"	{ DROP INDEX foo1_1_a; }

# In all permutations below, while "s1exec", "s1exec2", etc. wait to
# acquire the advisory lock, "s2drop" drops the index being used in the
# cached plan. When "s1exec" and others are unblocked and begin initializing
# the plan, including acquiring necessary locks on partitions, the concurrent
# index drop is detected. This causes plan initialization to be aborted,
# prompting the caller to retry with a new plan.

# Case with runtime pruning using EXTERN parameter
permutation "s1prep" "s2lock" "s1exec" "s2dropi" "s2unlock"

# Case with runtime pruning using stable function
permutation "s1prep2" "s2lock" "s1exec2" "s2dropi" "s2unlock"

# Case with a rule adding another query causing the CachedPlan to contain
# multiple PlannedStmts
permutation "s1prep3" "s2lock" "s1exec3" "s2dropi" "s2unlock"

# Case with run-time pruning inside a subquery
permutation "s1prep4" "s2lock" "s1exec4" "s2dropi" "s2unlock"
