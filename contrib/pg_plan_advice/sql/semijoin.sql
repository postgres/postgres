LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE sj_wide (
	id integer primary key,
	val1 integer,
	padding text storage plain
) WITH (autovacuum_enabled = false);
INSERT INTO sj_wide
	SELECT g, g%10+1, repeat(' ', 300) FROM generate_series(1, 1000) g;
CREATE INDEX ON sj_wide (val1);
VACUUM ANALYZE sj_wide;

CREATE TABLE sj_narrow (
	id integer primary key,
	val1 integer
) WITH (autovacuum_enabled = false);
INSERT INTO sj_narrow
	SELECT g, g%10+1 FROM generate_series(1, 1000) g;
CREATE INDEX ON sj_narrow (val1);
VACUUM ANALYZE sj_narrow;

-- We expect this to make the VALUES list unique and use index lookups to
-- find the rows in sj_wide, so as to avoid a full scan of sj_wide.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM sj_wide
	WHERE (id, val1) IN (VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5));

-- If we ask for a unique semijoin, we should get the same plan as with
-- no advice. If we ask for a non-unique semijoin, we should see a Semi
-- Join operation in the plan tree.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'semijoin_unique("*VALUES*")';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM sj_wide
	WHERE (id, val1) IN (VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5));
SET LOCAL pg_plan_advice.advice = 'semijoin_non_unique("*VALUES*")';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM sj_wide
	WHERE (id, val1) IN (VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5));
COMMIT;

-- Because this table is narrower than the previous one, a sequential scan
-- is less expensive, and we choose a straightforward Semi Join plan by
-- default. (Note that this is also very sensitive to the length of the IN
-- list, which affects how many index lookups the alternative plan will need.)
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM sj_narrow
	WHERE (id, val1) IN (VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5));

-- Here, we expect advising a unique semijoin to swith to the same plan that
-- we got with sj_wide, and advising a non-unique semijoin should not change
-- the plan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'semijoin_unique("*VALUES*")';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM sj_narrow
	WHERE (id, val1) IN (VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5));
SET LOCAL pg_plan_advice.advice = 'semijoin_non_unique("*VALUES*")';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM sj_narrow
	WHERE (id, val1) IN (VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5));
COMMIT;

-- In the above example, we made the outer side of the join unique, but here,
-- we should make the inner side unique.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);

-- We should be able to force a plan with or without the make-unique strategy,
-- with either side as the driving table.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'semijoin_unique(sj_narrow)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
SET LOCAL pg_plan_advice.advice = 'semijoin_non_unique(sj_narrow)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
SET LOCAL pg_plan_advice.advice = 'semijoin_unique(sj_narrow) join_order(sj_narrow)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
SET LOCAL pg_plan_advice.advice = 'semijoin_non_unique(sj_narrow) join_order(sj_narrow)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
COMMIT;

-- However, mentioning the wrong side of the join should result in an advice
-- failure.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'semijoin_unique(g)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
SET LOCAL pg_plan_advice.advice = 'semijoin_non_unique(g)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
COMMIT;

-- Test conflicting advice.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'semijoin_unique(sj_narrow) semijoin_non_unique(sj_narrow)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g
    WHERE g in (select val1 from sj_narrow);
COMMIT;

-- Try applying SEMIJOIN_UNIQUE() to a non-semijoin.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'semijoin_unique(g)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM generate_series(1,1000) g, sj_narrow s WHERE g = s.val1;
COMMIT;
