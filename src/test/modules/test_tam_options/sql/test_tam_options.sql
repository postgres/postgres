CREATE EXTENSION test_tam_options;

-- encourage use of parallel plans
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 4;

CREATE TABLE test (i int) USING heap_alter_options;

INSERT INTO test SELECT i FROM generate_series(1, 10000) i;
VACUUM ANALYZE test;

EXPLAIN (costs off)
SELECT * FROM test;

ALTER TABLE test SET (enable_parallel = OFF);

EXPLAIN (costs off)
SELECT * FROM test;

ALTER TABLE test SET (enable_parallel = ON);

EXPLAIN (costs off)
SELECT * FROM test;
