--
-- Tests for CPU time tracking
--
SET pg_stat_statements.track_utility = FALSE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Run a CPU-intensive query
SELECT count(*) FROM generate_series(1, 100000);

-- Verify CPU time columns are populated and sensible
SELECT query,
  total_exec_cpu_time > 0 AS has_cpu_time,
  total_exec_cpu_time <= total_exec_time AS cpu_le_wall
  FROM pg_stat_statements
  WHERE query LIKE '%generate_series%'
  ORDER BY query COLLATE "C";

-- Test planning CPU time
SET pg_stat_statements.track_planning = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

SELECT count(*) FROM generate_series(1, 100000);

SELECT query,
  total_exec_cpu_time >= 0 AS has_exec_cpu,
  total_plan_cpu_time >= 0 AS has_plan_cpu
  FROM pg_stat_statements
  WHERE query LIKE '%generate_series%'
  ORDER BY query COLLATE "C";

-- Cleanup
SET pg_stat_statements.track_planning = FALSE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
