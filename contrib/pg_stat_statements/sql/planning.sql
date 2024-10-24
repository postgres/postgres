--
-- Information related to planning
--

-- These tests require track_planning to be enabled.
SET pg_stat_statements.track_planning = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- [re]plan counting
--
CREATE TABLE stats_plan_test ();
PREPARE prep1 AS SELECT COUNT(*) FROM stats_plan_test;
EXECUTE prep1;
EXECUTE prep1;
EXECUTE prep1;
ALTER TABLE stats_plan_test ADD COLUMN x int;
EXECUTE prep1;
SELECT 42;
SELECT 42;
SELECT 42;
SELECT plans, calls, rows, query FROM pg_stat_statements
  WHERE query NOT LIKE 'SELECT COUNT%' ORDER BY query COLLATE "C";
-- for the prepared statement we expect at least one replan, but cache
-- invalidations could force more
SELECT plans >= 2 AND plans <= calls AS plans_ok, calls, rows, query FROM pg_stat_statements
  WHERE query LIKE 'SELECT COUNT%' ORDER BY query COLLATE "C";

-- Cleanup
DROP TABLE stats_plan_test;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
