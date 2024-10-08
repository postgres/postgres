--
-- Tests for parallel statistics
--

SET pg_stat_statements.track_utility = FALSE;

-- encourage use of parallel plans
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;

CREATE TABLE pgss_parallel_tab (a int);

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

SELECT count(*) FROM pgss_parallel_tab;

SELECT query,
  parallel_workers_to_launch > 0 AS has_workers_to_launch,
  parallel_workers_launched > 0 AS has_workers_launched
  FROM pg_stat_statements
  WHERE query ~ 'SELECT count'
  ORDER BY query COLLATE "C";

DROP TABLE pgss_parallel_tab;
