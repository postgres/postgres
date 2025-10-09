--
-- EAGER AGGREGATION
-- Test we can push aggregation down below join
--

CREATE TABLE eager_agg_t1 (a int, b int, c double precision);
CREATE TABLE eager_agg_t2 (a int, b int, c double precision);
CREATE TABLE eager_agg_t3 (a int, b int, c double precision);

INSERT INTO eager_agg_t1 SELECT i, i, i FROM generate_series(1, 1000) i;
INSERT INTO eager_agg_t2 SELECT i, i%10, i FROM generate_series(1, 1000) i;
INSERT INTO eager_agg_t3 SELECT i%10, i%10, i FROM generate_series(1, 1000) i;

ANALYZE eager_agg_t1;
ANALYZE eager_agg_t2;
ANALYZE eager_agg_t3;


--
-- Test eager aggregation over base rel
--

-- Perform scan of a table, aggregate the result, join it to the other table
-- and finalize the aggregation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

-- Produce results with sorting aggregation
SET enable_hashagg TO off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

RESET enable_hashagg;


--
-- Test eager aggregation over join rel
--

-- Perform join of tables, aggregate the result, join it to the other table
-- and finalize the aggregation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

-- Produce results with sorting aggregation
SET enable_hashagg TO off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

RESET enable_hashagg;


--
-- Test that eager aggregation works for outer join
--

-- Ensure aggregation can be pushed down to the non-nullable side
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  RIGHT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  RIGHT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

-- Ensure aggregation cannot be pushed down to the nullable side
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.b, avg(t2.c)
  FROM eager_agg_t1 t1
  LEFT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t2.b ORDER BY t2.b;

SELECT t2.b, avg(t2.c)
  FROM eager_agg_t1 t1
  LEFT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t2.b ORDER BY t2.b;


--
-- Test that eager aggregation works for parallel plans
--

SET parallel_setup_cost=0;
SET parallel_tuple_cost=0;
SET min_parallel_table_scan_size=0;
SET max_parallel_workers_per_gather=4;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

--
-- Test eager aggregation with GEQO
--

SET geqo = on;
SET geqo_threshold = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

RESET geqo;
RESET geqo_threshold;

DROP TABLE eager_agg_t1;
DROP TABLE eager_agg_t2;
DROP TABLE eager_agg_t3;


--
-- Test eager aggregation for partitionwise join
--

-- Enable partitionwise aggregate, which by default is disabled.
SET enable_partitionwise_aggregate TO true;
-- Enable partitionwise join, which by default is disabled.
SET enable_partitionwise_join TO true;

CREATE TABLE eager_agg_tab1(x int, y int) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab1_p1 PARTITION OF eager_agg_tab1 FOR VALUES FROM (0) TO (5);
CREATE TABLE eager_agg_tab1_p2 PARTITION OF eager_agg_tab1 FOR VALUES FROM (5) TO (10);
CREATE TABLE eager_agg_tab1_p3 PARTITION OF eager_agg_tab1 FOR VALUES FROM (10) TO (15);
CREATE TABLE eager_agg_tab2(x int, y int) PARTITION BY RANGE(y);
CREATE TABLE eager_agg_tab2_p1 PARTITION OF eager_agg_tab2 FOR VALUES FROM (0) TO (5);
CREATE TABLE eager_agg_tab2_p2 PARTITION OF eager_agg_tab2 FOR VALUES FROM (5) TO (10);
CREATE TABLE eager_agg_tab2_p3 PARTITION OF eager_agg_tab2 FOR VALUES FROM (10) TO (15);
INSERT INTO eager_agg_tab1 SELECT i % 15, i % 10 FROM generate_series(1, 1000) i;
INSERT INTO eager_agg_tab2 SELECT i % 10, i % 15 FROM generate_series(1, 1000) i;

ANALYZE eager_agg_tab1;
ANALYZE eager_agg_tab2;

-- When GROUP BY clause matches; full aggregation is performed for each
-- partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

-- GROUP BY having other matching key
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.y, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.y ORDER BY t2.y;

SELECT t2.y, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.y ORDER BY t2.y;

-- When GROUP BY clause does not match; partial aggregation is performed for
-- each partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.x, sum(t1.x), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.x HAVING avg(t1.x) > 5 ORDER BY t2.x;

SELECT t2.x, sum(t1.x), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.x HAVING avg(t1.x) > 5 ORDER BY t2.x;

-- Check with eager aggregation over join rel
-- full aggregation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

-- partial aggregation
SET enable_hashagg TO off;
SET max_parallel_workers_per_gather TO 0;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t3.y, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

SELECT t3.y, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

RESET enable_hashagg;
RESET max_parallel_workers_per_gather;

-- try that with GEQO too
SET geqo = on;
SET geqo_threshold = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

RESET geqo;
RESET geqo_threshold;

DROP TABLE eager_agg_tab1;
DROP TABLE eager_agg_tab2;


--
-- Test with multi-level partitioning scheme
--
CREATE TABLE eager_agg_tab_ml(x int, y int) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab_ml_p1 PARTITION OF eager_agg_tab_ml FOR VALUES FROM (0) TO (10);
CREATE TABLE eager_agg_tab_ml_p2 PARTITION OF eager_agg_tab_ml FOR VALUES FROM (10) TO (20) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab_ml_p2_s1 PARTITION OF eager_agg_tab_ml_p2 FOR VALUES FROM (10) TO (15);
CREATE TABLE eager_agg_tab_ml_p2_s2 PARTITION OF eager_agg_tab_ml_p2 FOR VALUES FROM (15) TO (20);
CREATE TABLE eager_agg_tab_ml_p3 PARTITION OF eager_agg_tab_ml FOR VALUES FROM (20) TO (30) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab_ml_p3_s1 PARTITION OF eager_agg_tab_ml_p3 FOR VALUES FROM (20) TO (25);
CREATE TABLE eager_agg_tab_ml_p3_s2 PARTITION OF eager_agg_tab_ml_p3 FOR VALUES FROM (25) TO (30);
INSERT INTO eager_agg_tab_ml SELECT i % 30, i % 30 FROM generate_series(1, 1000) i;

ANALYZE eager_agg_tab_ml;

-- When GROUP BY clause matches; full aggregation is performed for each
-- partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

-- When GROUP BY clause does not match; partial aggregation is performed for
-- each partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.y, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.y ORDER BY t1.y;

SELECT t1.y, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.y ORDER BY t1.y;

-- Check with eager aggregation over join rel
-- full aggregation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

-- partial aggregation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t3.y, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

SELECT t3.y, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

-- try that with GEQO too
SET geqo = on;
SET geqo_threshold = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

RESET geqo;
RESET geqo_threshold;

DROP TABLE eager_agg_tab_ml;
