LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 0.1;
SET random_page_cost = 0.1;
SET cpu_tuple_cost = 0;
SET cpu_index_tuple_cost = 0;

CREATE TABLE scan_table (a int primary key, b text)
	WITH (autovacuum_enabled = false);
INSERT INTO scan_table
	SELECT g, 'some text ' || g FROM generate_series(1, 100000) g;
CREATE INDEX scan_table_b ON scan_table USING brin (b);
VACUUM ANALYZE scan_table;

-- Sequential scan
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table;

-- Index scan
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;

-- Index-only scan
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;

-- Bitmap heap scan
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table
	WHERE b > 'some text 8';

-- TID scan
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE ctid = '(0,1)';

-- TID range scan
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table
	WHERE ctid > '(1,1)' AND ctid < '(2,1)';

-- Try forcing each of our test queries to use the scan type they
-- wanted to use anyway. This should succeed.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_ONLY_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'BITMAP_HEAP_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table
	WHERE b > 'some text 8';
SET LOCAL pg_plan_advice.advice = 'TID_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE ctid = '(0,1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table
	WHERE ctid > '(1,1)' AND ctid < '(2,1)';
COMMIT;

-- Try to force a full scan of the table to use some other scan type. All
-- of these will fail. An index scan or bitmap heap scan could potentially
-- generate the correct answer, but the planner does not even consider these
-- possibilities due to the lack of a WHERE clause.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table;
SET LOCAL pg_plan_advice.advice = 'INDEX_ONLY_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table;
SET LOCAL pg_plan_advice.advice = 'BITMAP_HEAP_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table;
SET LOCAL pg_plan_advice.advice = 'TID_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table;
COMMIT;

-- Try again to force index use. This should now succeed for the INDEX_SCAN
-- and BITMAP_HEAP_SCAN, but the INDEX_ONLY_SCAN can't be forced because the
-- query fetches columns not included in the index.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a > 0;
SET LOCAL pg_plan_advice.advice = 'INDEX_ONLY_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a > 0;
SET LOCAL pg_plan_advice.advice = 'BITMAP_HEAP_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a > 0;
COMMIT;

-- We can force a primary key lookup to use a sequential scan, but we
-- can't force it to use an index-only scan (due to the column list)
-- or a TID scan (due to the absence of a TID qual).
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_ONLY_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'TID_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
COMMIT;

-- We can forcibly downgrade an index-only scan to an index scan, but we can't
-- force the use of an index that the planner thinks is inapplicable.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table public.scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_b)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
COMMIT;

-- We can force the use of a sequential scan in place of a bitmap heap scan,
-- but a plain index scan on a BRIN index is not possible.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table
	WHERE b > 'some text 8';
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_b)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
COMMIT;

-- We can force the use of a sequential scan rather than a TID scan or
-- TID range scan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE ctid = '(0,1)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table
	WHERE ctid > '(1,1)' AND ctid < '(2,1)';
COMMIT;

-- Test more complex scenarios with index scans.
BEGIN;
-- Should still work if we mention the schema.
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table public.scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
-- But not if we mention the wrong schema.
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table cilbup.scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
-- It's OK to repeat the same advice.
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_pkey scan_table scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
-- But it doesn't work if the index target is even notionally different.
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table scan_table_pkey scan_table public.scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT * FROM scan_table WHERE a = 1;
COMMIT;

-- Test assorted incorrect advice.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(nothing)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(nothing whatsoever)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_SCAN(scan_table bogus)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_ONLY_SCAN(nothing whatsoever)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;
SET LOCAL pg_plan_advice.advice = 'INDEX_ONLY_SCAN(scan_table bogus)';
EXPLAIN (COSTS OFF, PLAN_ADVICE) SELECT a FROM scan_table WHERE a = 1;
COMMIT;

-- Test our ability to refer to multiple instances of the same alias.
BEGIN;
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (generate_series(1,10) g LEFT JOIN scan_table s ON g = s.a) x
    LEFT JOIN scan_table s ON g = s.a;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(s)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (generate_series(1,10) g LEFT JOIN scan_table s ON g = s.a) x
    LEFT JOIN scan_table s ON g = s.a;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(s#2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (generate_series(1,10) g LEFT JOIN scan_table s ON g = s.a) x
    LEFT JOIN scan_table s ON g = s.a;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(s) SEQ_SCAN(s#2)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (generate_series(1,10) g LEFT JOIN scan_table s ON g = s.a) x
    LEFT JOIN scan_table s ON g = s.a;
COMMIT;

-- Test our ability to refer to scans within a subquery.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0) x;
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0);
BEGIN;
-- Should not match.
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(s)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0) x;
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0);
-- Should match first query only.
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(s@x)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0) x;
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0);
-- Should match second query only.
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(s@unnamed_subquery)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0) x;
EXPLAIN (COSTS OFF, PLAN_ADVICE)
SELECT * FROM (SELECT * FROM scan_table s WHERE a = 1 OFFSET 0);
COMMIT;
