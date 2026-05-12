--
-- Tests with plan cache
--

-- Setup
CREATE OR REPLACE FUNCTION select_one_func(int) RETURNS VOID AS $$
DECLARE
  ret INT;
BEGIN
  SELECT $1 INTO ret;
END;
$$ LANGUAGE plpgsql;
CREATE OR REPLACE PROCEDURE select_one_proc(int) AS $$
DECLARE
  ret INT;
BEGIN
  SELECT $1 INTO ret;
END;
$$ LANGUAGE plpgsql;

-- Prepared statements
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
PREPARE p1 AS SELECT $1 AS a;
SET plan_cache_mode TO force_generic_plan;
EXECUTE p1(1);
SET plan_cache_mode TO force_custom_plan;
EXECUTE p1(1);
SELECT calls, generic_plan_calls, custom_plan_calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";
DEALLOCATE p1;

-- Extended query protocol
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT $1 AS a \parse p1
SET plan_cache_mode TO force_generic_plan;
\bind_named p1 1
;
SET plan_cache_mode TO force_custom_plan;
\bind_named p1 1
;
SELECT calls, generic_plan_calls, custom_plan_calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";
\close_prepared p1

-- EXPLAIN [ANALYZE] EXECUTE
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
PREPARE p1 AS SELECT $1;
SET plan_cache_mode TO force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE p1(1);
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) EXECUTE p1(1);
SET plan_cache_mode TO force_custom_plan;
EXPLAIN (COSTS OFF) EXECUTE p1(1);
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) EXECUTE p1(1);
SELECT calls, generic_plan_calls, custom_plan_calls, toplevel, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";
RESET pg_stat_statements.track;
DEALLOCATE p1;

-- Functions/procedures
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SET plan_cache_mode TO force_generic_plan;
SELECT select_one_func(1);
CALL select_one_proc(1);
SET plan_cache_mode TO force_custom_plan;
SELECT select_one_func(1);
CALL select_one_proc(1);
SELECT calls, generic_plan_calls, custom_plan_calls, toplevel, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

--
-- EXPLAIN [ANALYZE] EXECUTE + functions/procedures
--
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SET plan_cache_mode TO force_generic_plan;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) SELECT select_one_func(1);
EXPLAIN (COSTS OFF) SELECT select_one_func(1);
CALL select_one_proc(1);
SET plan_cache_mode TO force_custom_plan;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) SELECT select_one_func(1);
EXPLAIN (COSTS OFF) SELECT select_one_func(1);
CALL select_one_proc(1);
SELECT calls, generic_plan_calls, custom_plan_calls, toplevel, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

RESET pg_stat_statements.track;

--
-- Procedure with internal ROLLBACK and the extended query protocol.
-- The PlannedStmt used in pgss_ProcessUtility() is freed by the internal
-- ROLLBACK.
--
CREATE OR REPLACE PROCEDURE rollback_proc(a INOUT int) AS $$
BEGIN
  ROLLBACK;
END;
$$ LANGUAGE plpgsql;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CALL rollback_proc($1) \parse stmt_rollback
\bind_named stmt_rollback 1 \g
\bind_named stmt_rollback 2 \g
SELECT calls, query FROM pg_stat_statements
  WHERE query LIKE '%rollback_proc%'
  ORDER BY query COLLATE "C";
DROP PROCEDURE rollback_proc;

--
-- Cleanup
--
DROP FUNCTION select_one_func(int);
DROP PROCEDURE select_one_proc(int);
