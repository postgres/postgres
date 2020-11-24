--
-- PARTITION_AGGREGATE
-- Test partitionwise aggregation on partitioned tables
--
-- Note: to ensure plan stability, it's a good idea to make the partitions of
-- any one partitioned table in this test all have different numbers of rows.
--

-- Enable partitionwise aggregate, which by default is disabled.
SET enable_partitionwise_aggregate TO true;
-- Enable partitionwise join, which by default is disabled.
SET enable_partitionwise_join TO true;
-- Disable parallel plans.
SET max_parallel_workers_per_gather TO 0;
-- Disable incremental sort, which can influence selected plans due to fuzz factor.
SET enable_incremental_sort TO off;

--
-- Tests for list partitioned tables.
--
CREATE TABLE pagg_tab (a int, b int, c text, d int) PARTITION BY LIST(c);
CREATE TABLE pagg_tab_p1 PARTITION OF pagg_tab FOR VALUES IN ('0000', '0001', '0002', '0003', '0004');
CREATE TABLE pagg_tab_p2 PARTITION OF pagg_tab FOR VALUES IN ('0005', '0006', '0007', '0008');
CREATE TABLE pagg_tab_p3 PARTITION OF pagg_tab FOR VALUES IN ('0009', '0010', '0011');
INSERT INTO pagg_tab SELECT i % 20, i % 30, to_char(i % 12, 'FM0000'), i % 30 FROM generate_series(0, 2999) i;
ANALYZE pagg_tab;

-- When GROUP BY clause matches; full aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT c, sum(a), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY c HAVING avg(d) < 15 ORDER BY 1, 2, 3;
SELECT c, sum(a), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY c HAVING avg(d) < 15 ORDER BY 1, 2, 3;

-- When GROUP BY clause does not match; partial aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT a, sum(b), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY a HAVING avg(d) < 15 ORDER BY 1, 2, 3;
SELECT a, sum(b), avg(b), count(*), min(a), max(b) FROM pagg_tab GROUP BY a HAVING avg(d) < 15 ORDER BY 1, 2, 3;

-- Check with multiple columns in GROUP BY
EXPLAIN (COSTS OFF)
SELECT a, c, count(*) FROM pagg_tab GROUP BY a, c;
-- Check with multiple columns in GROUP BY, order in GROUP BY is reversed
EXPLAIN (COSTS OFF)
SELECT a, c, count(*) FROM pagg_tab GROUP BY c, a;
-- Check with multiple columns in GROUP BY, order in target-list is reversed
EXPLAIN (COSTS OFF)
SELECT c, a, count(*) FROM pagg_tab GROUP BY a, c;

-- Test when input relation for grouping is dummy
EXPLAIN (COSTS OFF)
SELECT c, sum(a) FROM pagg_tab WHERE 1 = 2 GROUP BY c;
SELECT c, sum(a) FROM pagg_tab WHERE 1 = 2 GROUP BY c;
EXPLAIN (COSTS OFF)
SELECT c, sum(a) FROM pagg_tab WHERE c = 'x' GROUP BY c;
SELECT c, sum(a) FROM pagg_tab WHERE c = 'x' GROUP BY c;

-- Test GroupAggregate paths by disabling hash aggregates.
SET enable_hashagg TO false;

-- When GROUP BY clause matches full aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT c, sum(a), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;
SELECT c, sum(a), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;

-- When GROUP BY clause does not match; partial aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT a, sum(b), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;
SELECT a, sum(b), avg(b), count(*) FROM pagg_tab GROUP BY 1 HAVING avg(d) < 15 ORDER BY 1, 2, 3;

-- Test partitionwise grouping without any aggregates
EXPLAIN (COSTS OFF)
SELECT c FROM pagg_tab GROUP BY c ORDER BY 1;
SELECT c FROM pagg_tab GROUP BY c ORDER BY 1;
EXPLAIN (COSTS OFF)
SELECT a FROM pagg_tab WHERE a < 3 GROUP BY a ORDER BY 1;
SELECT a FROM pagg_tab WHERE a < 3 GROUP BY a ORDER BY 1;

RESET enable_hashagg;

-- ROLLUP, partitionwise aggregation does not apply
EXPLAIN (COSTS OFF)
SELECT c, sum(a) FROM pagg_tab GROUP BY rollup(c) ORDER BY 1, 2;

-- ORDERED SET within the aggregate.
-- Full aggregation; since all the rows that belong to the same group come
-- from the same partition, having an ORDER BY within the aggregate doesn't
-- make any difference.
EXPLAIN (COSTS OFF)
SELECT c, sum(b order by a) FROM pagg_tab GROUP BY c ORDER BY 1, 2;
-- Since GROUP BY clause does not match with PARTITION KEY; we need to do
-- partial aggregation. However, ORDERED SET are not partial safe and thus
-- partitionwise aggregation plan is not generated.
EXPLAIN (COSTS OFF)
SELECT a, sum(b order by a) FROM pagg_tab GROUP BY a ORDER BY 1, 2;


-- JOIN query

CREATE TABLE pagg_tab1(x int, y int) PARTITION BY RANGE(x);
CREATE TABLE pagg_tab1_p1 PARTITION OF pagg_tab1 FOR VALUES FROM (0) TO (10);
CREATE TABLE pagg_tab1_p2 PARTITION OF pagg_tab1 FOR VALUES FROM (10) TO (20);
CREATE TABLE pagg_tab1_p3 PARTITION OF pagg_tab1 FOR VALUES FROM (20) TO (30);

CREATE TABLE pagg_tab2(x int, y int) PARTITION BY RANGE(y);
CREATE TABLE pagg_tab2_p1 PARTITION OF pagg_tab2 FOR VALUES FROM (0) TO (10);
CREATE TABLE pagg_tab2_p2 PARTITION OF pagg_tab2 FOR VALUES FROM (10) TO (20);
CREATE TABLE pagg_tab2_p3 PARTITION OF pagg_tab2 FOR VALUES FROM (20) TO (30);

INSERT INTO pagg_tab1 SELECT i % 30, i % 20 FROM generate_series(0, 299, 2) i;
INSERT INTO pagg_tab2 SELECT i % 20, i % 30 FROM generate_series(0, 299, 3) i;

ANALYZE pagg_tab1;
ANALYZE pagg_tab2;

-- When GROUP BY clause matches; full aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT t1.x, sum(t1.y), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;
SELECT t1.x, sum(t1.y), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;

-- Check with whole-row reference; partitionwise aggregation does not apply
EXPLAIN (COSTS OFF)
SELECT t1.x, sum(t1.y), count(t1) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;
SELECT t1.x, sum(t1.y), count(t1) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.x ORDER BY 1, 2, 3;

-- GROUP BY having other matching key
EXPLAIN (COSTS OFF)
SELECT t2.y, sum(t1.y), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t2.y ORDER BY 1, 2, 3;

-- When GROUP BY clause does not match; partial aggregation is performed for each partition.
-- Also test GroupAggregate paths by disabling hash aggregates.
SET enable_hashagg TO false;
EXPLAIN (COSTS OFF)
SELECT t1.y, sum(t1.x), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.y HAVING avg(t1.x) > 10 ORDER BY 1, 2, 3;
SELECT t1.y, sum(t1.x), count(*) FROM pagg_tab1 t1, pagg_tab2 t2 WHERE t1.x = t2.y GROUP BY t1.y HAVING avg(t1.x) > 10 ORDER BY 1, 2, 3;
RESET enable_hashagg;

-- Check with LEFT/RIGHT/FULL OUTER JOINs which produces NULL values for
-- aggregation

-- LEFT JOIN, should produce partial partitionwise aggregation plan as
-- GROUP BY is on nullable column
EXPLAIN (COSTS OFF)
SELECT b.y, sum(a.y) FROM pagg_tab1 a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;
SELECT b.y, sum(a.y) FROM pagg_tab1 a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;

-- RIGHT JOIN, should produce full partitionwise aggregation plan as
-- GROUP BY is on non-nullable column
EXPLAIN (COSTS OFF)
SELECT b.y, sum(a.y) FROM pagg_tab1 a RIGHT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;
SELECT b.y, sum(a.y) FROM pagg_tab1 a RIGHT JOIN pagg_tab2 b ON a.x = b.y GROUP BY b.y ORDER BY 1 NULLS LAST;

-- FULL JOIN, should produce partial partitionwise aggregation plan as
-- GROUP BY is on nullable column
EXPLAIN (COSTS OFF)
SELECT a.x, sum(b.x) FROM pagg_tab1 a FULL OUTER JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x ORDER BY 1 NULLS LAST;
SELECT a.x, sum(b.x) FROM pagg_tab1 a FULL OUTER JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x ORDER BY 1 NULLS LAST;

-- LEFT JOIN, with dummy relation on right side, ideally
-- should produce full partitionwise aggregation plan as GROUP BY is on
-- non-nullable columns.
-- But right now we are unable to do partitionwise join in this case.
EXPLAIN (COSTS OFF)
SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a LEFT JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20  GROUP BY a.x, b.y ORDER BY 1, 2;
SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a LEFT JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20  GROUP BY a.x, b.y ORDER BY 1, 2;

-- FULL JOIN, with dummy relations on both sides, ideally
-- should produce partial partitionwise aggregation plan as GROUP BY is on
-- nullable columns.
-- But right now we are unable to do partitionwise join in this case.
EXPLAIN (COSTS OFF)
SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a FULL JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20  GROUP BY a.x, b.y ORDER BY 1, 2;
SELECT a.x, b.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x < 20) a FULL JOIN (SELECT * FROM pagg_tab2 WHERE y > 10) b ON a.x = b.y WHERE a.x > 5 or b.y < 20 GROUP BY a.x, b.y ORDER BY 1, 2;

-- Empty join relation because of empty outer side, no partitionwise agg plan
EXPLAIN (COSTS OFF)
SELECT a.x, a.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x = 1 AND x = 2) a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x, a.y ORDER BY 1, 2;
SELECT a.x, a.y, count(*) FROM (SELECT * FROM pagg_tab1 WHERE x = 1 AND x = 2) a LEFT JOIN pagg_tab2 b ON a.x = b.y GROUP BY a.x, a.y ORDER BY 1, 2;


-- Partition by multiple columns

CREATE TABLE pagg_tab_m (a int, b int, c int) PARTITION BY RANGE(a, ((a+b)/2));
CREATE TABLE pagg_tab_m_p1 PARTITION OF pagg_tab_m FOR VALUES FROM (0, 0) TO (12, 12);
CREATE TABLE pagg_tab_m_p2 PARTITION OF pagg_tab_m FOR VALUES FROM (12, 12) TO (22, 22);
CREATE TABLE pagg_tab_m_p3 PARTITION OF pagg_tab_m FOR VALUES FROM (22, 22) TO (30, 30);
INSERT INTO pagg_tab_m SELECT i % 30, i % 40, i % 50 FROM generate_series(0, 2999) i;
ANALYZE pagg_tab_m;

-- Partial aggregation as GROUP BY clause does not match with PARTITION KEY
EXPLAIN (COSTS OFF)
SELECT a, sum(b), avg(c), count(*) FROM pagg_tab_m GROUP BY a HAVING avg(c) < 22 ORDER BY 1, 2, 3;
SELECT a, sum(b), avg(c), count(*) FROM pagg_tab_m GROUP BY a HAVING avg(c) < 22 ORDER BY 1, 2, 3;

-- Full aggregation as GROUP BY clause matches with PARTITION KEY
EXPLAIN (COSTS OFF)
SELECT a, sum(b), avg(c), count(*) FROM pagg_tab_m GROUP BY a, (a+b)/2 HAVING sum(b) < 50 ORDER BY 1, 2, 3;
SELECT a, sum(b), avg(c), count(*) FROM pagg_tab_m GROUP BY a, (a+b)/2 HAVING sum(b) < 50 ORDER BY 1, 2, 3;

-- Full aggregation as PARTITION KEY is part of GROUP BY clause
EXPLAIN (COSTS OFF)
SELECT a, c, sum(b), avg(c), count(*) FROM pagg_tab_m GROUP BY (a+b)/2, 2, 1 HAVING sum(b) = 50 AND avg(c) > 25 ORDER BY 1, 2, 3;
SELECT a, c, sum(b), avg(c), count(*) FROM pagg_tab_m GROUP BY (a+b)/2, 2, 1 HAVING sum(b) = 50 AND avg(c) > 25 ORDER BY 1, 2, 3;


-- Test with multi-level partitioning scheme

CREATE TABLE pagg_tab_ml (a int, b int, c text) PARTITION BY RANGE(a);
CREATE TABLE pagg_tab_ml_p1 PARTITION OF pagg_tab_ml FOR VALUES FROM (0) TO (12);
CREATE TABLE pagg_tab_ml_p2 PARTITION OF pagg_tab_ml FOR VALUES FROM (12) TO (20) PARTITION BY LIST (c);
CREATE TABLE pagg_tab_ml_p2_s1 PARTITION OF pagg_tab_ml_p2 FOR VALUES IN ('0000', '0001', '0002');
CREATE TABLE pagg_tab_ml_p2_s2 PARTITION OF pagg_tab_ml_p2 FOR VALUES IN ('0003');

-- This level of partitioning has different column positions than the parent
CREATE TABLE pagg_tab_ml_p3(b int, c text, a int) PARTITION BY RANGE (b);
CREATE TABLE pagg_tab_ml_p3_s1(c text, a int, b int);
CREATE TABLE pagg_tab_ml_p3_s2 PARTITION OF pagg_tab_ml_p3 FOR VALUES FROM (7) TO (10);

ALTER TABLE pagg_tab_ml_p3 ATTACH PARTITION pagg_tab_ml_p3_s1 FOR VALUES FROM (0) TO (7);
ALTER TABLE pagg_tab_ml ATTACH PARTITION pagg_tab_ml_p3 FOR VALUES FROM (20) TO (30);

INSERT INTO pagg_tab_ml SELECT i % 30, i % 10, to_char(i % 4, 'FM0000') FROM generate_series(0, 29999) i;
ANALYZE pagg_tab_ml;

-- For Parallel Append
SET max_parallel_workers_per_gather TO 2;

-- Full aggregation at level 1 as GROUP BY clause matches with PARTITION KEY
-- for level 1 only. For subpartitions, GROUP BY clause does not match with
-- PARTITION KEY, but still we do not see a partial aggregation as array_agg()
-- is not partial agg safe.
EXPLAIN (COSTS OFF)
SELECT a, sum(b), array_agg(distinct c), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3 ORDER BY 1, 2, 3;
SELECT a, sum(b), array_agg(distinct c), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3 ORDER BY 1, 2, 3;

-- Without ORDER BY clause, to test Gather at top-most path
EXPLAIN (COSTS OFF)
SELECT a, sum(b), array_agg(distinct c), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3;

-- Full aggregation at level 1 as GROUP BY clause matches with PARTITION KEY
-- for level 1 only. For subpartitions, GROUP BY clause does not match with
-- PARTITION KEY, thus we will have a partial aggregation for them.
EXPLAIN (COSTS OFF)
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3 ORDER BY 1, 2, 3;
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3 ORDER BY 1, 2, 3;

-- Partial aggregation at all levels as GROUP BY clause does not match with
-- PARTITION KEY
EXPLAIN (COSTS OFF)
SELECT b, sum(a), count(*) FROM pagg_tab_ml GROUP BY b ORDER BY 1, 2, 3;
SELECT b, sum(a), count(*) FROM pagg_tab_ml GROUP BY b HAVING avg(a) < 15 ORDER BY 1, 2, 3;

-- Full aggregation at all levels as GROUP BY clause matches with PARTITION KEY
EXPLAIN (COSTS OFF)
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a, b, c HAVING avg(b) > 7 ORDER BY 1, 2, 3;
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a, b, c HAVING avg(b) > 7 ORDER BY 1, 2, 3;

-- Parallelism within partitionwise aggregates

SET min_parallel_table_scan_size TO '8kB';
SET parallel_setup_cost TO 0;

-- Full aggregation at level 1 as GROUP BY clause matches with PARTITION KEY
-- for level 1 only. For subpartitions, GROUP BY clause does not match with
-- PARTITION KEY, thus we will have a partial aggregation for them.
EXPLAIN (COSTS OFF)
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3 ORDER BY 1, 2, 3;
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a HAVING avg(b) < 3 ORDER BY 1, 2, 3;

-- Partial aggregation at all levels as GROUP BY clause does not match with
-- PARTITION KEY
EXPLAIN (COSTS OFF)
SELECT b, sum(a), count(*) FROM pagg_tab_ml GROUP BY b ORDER BY 1, 2, 3;
SELECT b, sum(a), count(*) FROM pagg_tab_ml GROUP BY b HAVING avg(a) < 15 ORDER BY 1, 2, 3;

-- Full aggregation at all levels as GROUP BY clause matches with PARTITION KEY
EXPLAIN (COSTS OFF)
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a, b, c HAVING avg(b) > 7 ORDER BY 1, 2, 3;
SELECT a, sum(b), count(*) FROM pagg_tab_ml GROUP BY a, b, c HAVING avg(b) > 7 ORDER BY 1, 2, 3;


-- Parallelism within partitionwise aggregates (single level)

-- Add few parallel setup cost, so that we will see a plan which gathers
-- partially created paths even for full aggregation and sticks a single Gather
-- followed by finalization step.
-- Without this, the cost of doing partial aggregation + Gather + finalization
-- for each partition and then Append over it turns out to be same and this
-- wins as we add it first. This parallel_setup_cost plays a vital role in
-- costing such plans.
SET parallel_setup_cost TO 10;

CREATE TABLE pagg_tab_para(x int, y int) PARTITION BY RANGE(x);
CREATE TABLE pagg_tab_para_p1 PARTITION OF pagg_tab_para FOR VALUES FROM (0) TO (12);
CREATE TABLE pagg_tab_para_p2 PARTITION OF pagg_tab_para FOR VALUES FROM (12) TO (22);
CREATE TABLE pagg_tab_para_p3 PARTITION OF pagg_tab_para FOR VALUES FROM (22) TO (30);

INSERT INTO pagg_tab_para SELECT i % 30, i % 20 FROM generate_series(0, 29999) i;

ANALYZE pagg_tab_para;

-- When GROUP BY clause matches; full aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT x, sum(y), avg(y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;
SELECT x, sum(y), avg(y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;

-- When GROUP BY clause does not match; partial aggregation is performed for each partition.
EXPLAIN (COSTS OFF)
SELECT y, sum(x), avg(x), count(*) FROM pagg_tab_para GROUP BY y HAVING avg(x) < 12 ORDER BY 1, 2, 3;
SELECT y, sum(x), avg(x), count(*) FROM pagg_tab_para GROUP BY y HAVING avg(x) < 12 ORDER BY 1, 2, 3;

-- Test when parent can produce parallel paths but not any (or some) of its children
-- (Use one more aggregate to tilt the cost estimates for the plan we want)
ALTER TABLE pagg_tab_para_p1 SET (parallel_workers = 0);
ALTER TABLE pagg_tab_para_p3 SET (parallel_workers = 0);
ANALYZE pagg_tab_para;

EXPLAIN (COSTS OFF)
SELECT x, sum(y), avg(y), sum(x+y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;
SELECT x, sum(y), avg(y), sum(x+y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;

ALTER TABLE pagg_tab_para_p2 SET (parallel_workers = 0);
ANALYZE pagg_tab_para;

EXPLAIN (COSTS OFF)
SELECT x, sum(y), avg(y), sum(x+y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;
SELECT x, sum(y), avg(y), sum(x+y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;

-- Reset parallelism parameters to get partitionwise aggregation plan.
RESET min_parallel_table_scan_size;
RESET parallel_setup_cost;

EXPLAIN (COSTS OFF)
SELECT x, sum(y), avg(y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;
SELECT x, sum(y), avg(y), count(*) FROM pagg_tab_para GROUP BY x HAVING avg(y) < 7 ORDER BY 1, 2, 3;
