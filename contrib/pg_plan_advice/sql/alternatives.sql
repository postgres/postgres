LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE alt_t1 (a int) WITH (autovacuum_enabled = false);
CREATE TABLE alt_t2 (a int) WITH (autovacuum_enabled = false);
CREATE INDEX ON alt_t2(a);
INSERT INTO alt_t1 SELECT generate_series(1, 1000);
INSERT INTO alt_t2 SELECT generate_series(1, 100000);
VACUUM ANALYZE alt_t1;
VACUUM ANALYZE alt_t2;

-- This query uses an OR to prevent the EXISTS from being converted to a
-- semi-join, forcing the planner through the AlternativeSubPlan path.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM alt_t1
WHERE EXISTS (SELECT 1 FROM alt_t2 WHERE alt_t2.a = alt_t1.a) OR alt_t1.a < 0;

-- We should be able to force either AlternativeSubPlan by advising against
-- scanning the other relation.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'DO_NOT_SCAN(alt_t2@exists_1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM alt_t1
WHERE EXISTS (SELECT 1 FROM alt_t2 WHERE alt_t2.a = alt_t1.a) OR alt_t1.a < 0;
SET LOCAL pg_plan_advice.advice = 'DO_NOT_SCAN(alt_t2@exists_2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM alt_t1
WHERE EXISTS (SELECT 1 FROM alt_t2 WHERE alt_t2.a = alt_t1.a) OR alt_t1.a < 0;
COMMIT;

-- Now let's test a case involving MinMaxAggPath, which we treat similarly
-- to the AlternativeSubPlan case.
CREATE TABLE alt_minmax (a int) WITH (autovacuum_enabled = false);
CREATE INDEX ON alt_minmax(a);
INSERT INTO alt_minmax SELECT generate_series(1, 10000);
VACUUM ANALYZE alt_minmax;

-- Using an Index Scan inside of an InitPlan should win over a full table
-- scan.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT min(a), max(a) FROM alt_minmax;

-- Advising against the scan of alt_minmax at the root query level should
-- change nothing, but if we say we don't want either of or both of the
-- minmax-variant scans, the plan should switch to a full table scan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'DO_NOT_SCAN(alt_minmax)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT min(a), max(a) FROM alt_minmax;
SET LOCAL pg_plan_advice.advice = 'DO_NOT_SCAN(alt_minmax@minmax_1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT min(a), max(a) FROM alt_minmax;
SET LOCAL pg_plan_advice.advice = 'DO_NOT_SCAN(alt_minmax@minmax_1) DO_NOT_SCAN(alt_minmax@minmax_2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT min(a), max(a) FROM alt_minmax;
COMMIT;

DROP TABLE alt_t1, alt_t2, alt_minmax;
