--
-- Statement level tracking
--

SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DO block - top-level tracking.
CREATE TABLE stats_track_tab (x int);
SET pg_stat_statements.track = 'top';
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END;
$$ LANGUAGE plpgsql;
SELECT toplevel, calls, query FROM pg_stat_statements
  WHERE query LIKE '%DELETE%' ORDER BY query COLLATE "C", toplevel;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DO block - all-level tracking.
SET pg_stat_statements.track = 'all';
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END; $$;
DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END; $$;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- Procedure with multiple utility statements.
CREATE OR REPLACE PROCEDURE proc_with_utility_stmt()
LANGUAGE SQL
AS $$
  SHOW pg_stat_statements.track;
  show pg_stat_statements.track;
  SHOW pg_stat_statements.track_utility;
$$;
SET pg_stat_statements.track_utility = TRUE;
-- all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CALL proc_with_utility_stmt();
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;
-- top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CALL proc_with_utility_stmt();
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- EXPLAIN - all-level tracking.
CREATE TABLE test_table (x int);
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) SELECT 1;
EXPLAIN (COSTS OFF) (SELECT 1, 2);
EXPLAIN (COSTS OFF) TABLE stats_track_tab;
EXPLAIN (COSTS OFF) (TABLE test_table);
EXPLAIN (COSTS OFF) VALUES (1);
EXPLAIN (COSTS OFF) (VALUES (1, 2));
EXPLAIN (COSTS OFF) UPDATE stats_track_tab SET x = 1 WHERE x = 1;
EXPLAIN (COSTS OFF) DELETE FROM stats_track_tab;
EXPLAIN (COSTS OFF) INSERT INTO stats_track_tab VALUES ((1));
EXPLAIN (COSTS OFF) MERGE INTO stats_track_tab
  USING (SELECT id FROM generate_series(1, 10) id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id);
EXPLAIN (COSTS OFF) SELECT 1 UNION SELECT 2;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN - top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) SELECT 1;
EXPLAIN (COSTS OFF) (SELECT 1, 2);
EXPLAIN (COSTS OFF) TABLE stats_track_tab;
EXPLAIN (COSTS OFF) (TABLE test_table);
EXPLAIN (COSTS OFF) VALUES (1);
EXPLAIN (COSTS OFF) (VALUES (1, 2));
EXPLAIN (COSTS OFF) UPDATE stats_track_tab SET x = 1 WHERE x = 1;
EXPLAIN (COSTS OFF) DELETE FROM stats_track_tab;
EXPLAIN (COSTS OFF) INSERT INTO stats_track_tab VALUES ((1));
EXPLAIN (COSTS OFF) MERGE INTO stats_track_tab
  USING (SELECT id FROM generate_series(1, 10) id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id);
EXPLAIN (COSTS OFF) SELECT 1 UNION SELECT 2;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN - all-level tracking with multi-statement strings.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- SELECT queries
EXPLAIN (COSTS OFF) SELECT 1\; EXPLAIN (COSTS OFF) SELECT 1, 2;
EXPLAIN (COSTS OFF) (SELECT 1, 2, 3)\; EXPLAIN (COSTS OFF) (SELECT 1, 2, 3, 4);
EXPLAIN (COSTS OFF) SELECT 1, 2 UNION SELECT 3, 4\; EXPLAIN (COSTS OFF) (SELECT 1, 2, 3) UNION SELECT 3, 4, 5;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Most DMLs
EXPLAIN (COSTS OFF) TABLE stats_track_tab\; EXPLAIN (COSTS OFF) (TABLE test_table);
EXPLAIN (COSTS OFF) VALUES (1)\; EXPLAIN (COSTS OFF) (VALUES (1, 2));
EXPLAIN (COSTS OFF) UPDATE stats_track_tab SET x = 1 WHERE x = 1\; EXPLAIN (COSTS OFF) UPDATE stats_track_tab SET x = 1;
EXPLAIN (COSTS OFF) DELETE FROM stats_track_tab\; EXPLAIN (COSTS OFF) DELETE FROM stats_track_tab WHERE x = 1;
EXPLAIN (COSTS OFF) INSERT INTO stats_track_tab VALUES ((1))\; EXPLAIN (COSTS OFF) INSERT INTO stats_track_tab VALUES (1), (2);
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- MERGE, worth its own.
EXPLAIN (COSTS OFF) MERGE INTO stats_track_tab
  USING (SELECT id FROM generate_series(1, 10) id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id)\; EXPLAIN (COSTS OFF) SELECT 1, 2, 3, 4, 5;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN - top-level tracking with multi-statement strings.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) SELECT 1\; EXPLAIN (COSTS OFF) SELECT 1, 2;
EXPLAIN (COSTS OFF) (SELECT 1, 2, 3)\; EXPLAIN (COSTS OFF) (SELECT 1, 2, 3, 4);
EXPLAIN (COSTS OFF) TABLE stats_track_tab\; EXPLAIN (COSTS OFF) (TABLE test_table);
EXPLAIN (COSTS OFF) VALUES (1)\; EXPLAIN (COSTS OFF) (VALUES (1, 2));
EXPLAIN (COSTS OFF) UPDATE stats_track_tab SET x = 1 WHERE x = 1\; EXPLAIN (COSTS OFF) UPDATE stats_track_tab SET x = 1;
EXPLAIN (COSTS OFF) DELETE FROM stats_track_tab\; EXPLAIN (COSTS OFF) DELETE FROM stats_track_tab WHERE x = 1;
EXPLAIN (COSTS OFF) INSERT INTO stats_track_tab VALUES ((1))\; EXPLAIN (COSTS OFF) INSERT INTO stats_track_tab VALUES ((1), (2));
EXPLAIN (COSTS OFF) MERGE INTO stats_track_tab USING (SELECT id FROM generate_series(1, 10) id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id)\; EXPLAIN (COSTS OFF) SELECT 1, 2, 3, 4, 5;
EXPLAIN (COSTS OFF) SELECT 1, 2 UNION SELECT 3, 4\; EXPLAIN (COSTS OFF) (SELECT 1, 2, 3) UNION SELECT 3, 4, 5;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN with CTEs - all-level tracking
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) SELECT 1;
EXPLAIN (COSTS OFF) (WITH a AS (SELECT 4) (SELECT 1, 2));
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) UPDATE stats_track_tab SET x = 1 WHERE x = 1;
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) DELETE FROM stats_track_tab;
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) INSERT INTO stats_track_tab VALUES ((1));
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) MERGE INTO stats_track_tab
  USING (SELECT id FROM generate_series(1, 10) id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id);
EXPLAIN (COSTS OFF) WITH a AS (select 4) SELECT 1 UNION SELECT 2;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN with CTEs - top-level tracking
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) SELECT 1;
EXPLAIN (COSTS OFF) (WITH a AS (SELECT 4) (SELECT 1, 2));
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) UPDATE stats_track_tab SET x = 1 WHERE x = 1;
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) DELETE FROM stats_track_tab;
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) INSERT INTO stats_track_tab VALUES ((1));
EXPLAIN (COSTS OFF) WITH a AS (SELECT 4) MERGE INTO stats_track_tab
  USING (SELECT id FROM generate_series(1, 10) id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id);
EXPLAIN (COSTS OFF) WITH a AS (select 4) SELECT 1 UNION SELECT 2;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- Explain analyze, all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) SELECT 100;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
  DECLARE foocur CURSOR FOR SELECT * FROM stats_track_tab;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- Explain analyze, top tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF) SELECT 100;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
  DECLARE foocur CURSOR FOR SELECT * FROM stats_track_tab;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- Create Materialized View, all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE MATERIALIZED VIEW pgss_materialized_view AS
  SELECT * FROM generate_series(1, 5) as id;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- CREATE MATERIALIZED VIEW, top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE MATERIALIZED VIEW pgss_materialized_view_2 AS
  SELECT * FROM generate_series(1, 5) as id;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- REFRESH MATERIALIZED VIEW, all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
REFRESH MATERIALIZED VIEW pgss_materialized_view;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- REFRESH MATERIALIZED VIEW, top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
REFRESH MATERIALIZED VIEW pgss_materialized_view;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- CREATE TABLE AS, all-level tracking.
SET pg_stat_statements.track = 'all';
PREPARE test_prepare_pgss AS select generate_series(1, 10);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE TEMPORARY TABLE pgss_ctas_1 AS SELECT 1;
CREATE TEMPORARY TABLE pgss_ctas_2 AS EXECUTE test_prepare_pgss;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- CREATE TABLE AS, top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE TEMPORARY TABLE pgss_ctas_3 AS SELECT 1;
CREATE TEMPORARY TABLE pgss_ctas_4 AS EXECUTE test_prepare_pgss;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN with CREATE TABLE AS - all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) CREATE TEMPORARY TABLE pgss_explain_ctas AS SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- EXPLAIN with CREATE TABLE AS - top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
EXPLAIN (COSTS OFF) CREATE TEMPORARY TABLE pgss_explain_ctas AS SELECT 1;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- DECLARE CURSOR, all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
BEGIN;
DECLARE FOOCUR CURSOR FOR SELECT * from stats_track_tab;
FETCH FORWARD 1 FROM foocur;
CLOSE foocur;
COMMIT;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- DECLARE CURSOR, top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
BEGIN;
DECLARE FOOCUR CURSOR FOR SELECT * FROM stats_track_tab;
FETCH FORWARD 1 FROM foocur;
CLOSE foocur;
COMMIT;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- COPY - all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
COPY (SELECT 1) TO stdout;
COPY (SELECT 1 UNION SELECT 2) TO stdout;
COPY (MERGE INTO stats_track_tab USING (SELECT 1 id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id) RETURNING x) TO stdout;
COPY (INSERT INTO stats_track_tab (x) VALUES (1) RETURNING x) TO stdout;
COPY (UPDATE stats_track_tab SET x = 2 WHERE x = 1 RETURNING x) TO stdout;
COPY (DELETE FROM stats_track_tab WHERE x = 2 RETURNING x) TO stdout;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- COPY - top-level tracking.
SET pg_stat_statements.track = 'top';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
COPY (SELECT 1) TO stdout;
COPY (SELECT 1 UNION SELECT 2) TO stdout;
COPY (MERGE INTO stats_track_tab USING (SELECT 1 id) ON x = id
  WHEN MATCHED THEN UPDATE SET x = id
  WHEN NOT MATCHED THEN INSERT (x) VALUES (id) RETURNING x) TO stdout;
COPY (INSERT INTO stats_track_tab (x) VALUES (1) RETURNING x) TO stdout;
COPY (UPDATE stats_track_tab SET x = 2 WHERE x = 1 RETURNING x) TO stdout;
COPY (DELETE FROM stats_track_tab WHERE x = 2 RETURNING x) TO stdout;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C";

-- DO block - top-level tracking without utility.
SET pg_stat_statements.track = 'top';
SET pg_stat_statements.track_utility = FALSE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END; $$;
DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END; $$;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- DO block - all-level tracking without utility.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
DELETE FROM stats_track_tab;
DO $$
BEGIN
  DELETE FROM stats_track_tab;
END; $$;
DO LANGUAGE plpgsql $$
BEGIN
  -- this is a SELECT
  PERFORM 'hello world'::TEXT;
END; $$;
SELECT toplevel, calls, query FROM pg_stat_statements
  ORDER BY query COLLATE "C", toplevel;

-- DO block --- multiple inner queries with separators
SET pg_stat_statements.track = 'all';
SET pg_stat_statements.track_utility = TRUE;
CREATE TABLE pgss_do_util_tab_1 (a int);
CREATE TABLE pgss_do_util_tab_2 (a int);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
DO $$
DECLARE BEGIN
  EXECUTE 'CREATE TABLE pgss_do_table (id INT); DROP TABLE pgss_do_table';
  EXECUTE 'SELECT a FROM pgss_do_util_tab_1; SELECT a FROM pgss_do_util_tab_2';
END $$;
SELECT toplevel, calls, rows, query FROM pg_stat_statements
  WHERE toplevel IS FALSE
  ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Note the extra semicolon at the end of the query.
DO $$
DECLARE BEGIN
  EXECUTE 'CREATE TABLE pgss_do_table (id INT); DROP TABLE pgss_do_table;';
  EXECUTE 'SELECT a FROM pgss_do_util_tab_1; SELECT a FROM pgss_do_util_tab_2;';
END $$;
SELECT toplevel, calls, rows, query FROM pg_stat_statements
  WHERE toplevel IS FALSE
  ORDER BY query COLLATE "C";
DROP TABLE pgss_do_util_tab_1, pgss_do_util_tab_2;

-- PL/pgSQL function - top-level tracking.
SET pg_stat_statements.track = 'top';
SET pg_stat_statements.track_utility = FALSE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
CREATE FUNCTION PLUS_TWO(i INTEGER) RETURNS INTEGER AS $$
DECLARE
  r INTEGER;
BEGIN
  SELECT (i + 1 + 1.0)::INTEGER INTO r;
  RETURN r;
END; $$ LANGUAGE plpgsql;

SELECT PLUS_TWO(3);
SELECT PLUS_TWO(7);

-- SQL function --- use LIMIT to keep it from being inlined
CREATE FUNCTION PLUS_ONE(i INTEGER) RETURNS INTEGER AS
$$ SELECT (i + 1.0)::INTEGER LIMIT 1 $$ LANGUAGE SQL;

SELECT PLUS_ONE(8);
SELECT PLUS_ONE(10);

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- immutable SQL function --- can be executed at plan time
CREATE FUNCTION PLUS_THREE(i INTEGER) RETURNS INTEGER AS
$$ SELECT i + 3 LIMIT 1 $$ IMMUTABLE LANGUAGE SQL;

SELECT PLUS_THREE(8);
SELECT PLUS_THREE(10);

SELECT toplevel, calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- PL/pgSQL function - all-level tracking.
SET pg_stat_statements.track = 'all';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- we drop and recreate the functions to avoid any caching funnies
DROP FUNCTION PLUS_ONE(INTEGER);
DROP FUNCTION PLUS_TWO(INTEGER);
DROP FUNCTION PLUS_THREE(INTEGER);

-- PL/pgSQL function
CREATE FUNCTION PLUS_TWO(i INTEGER) RETURNS INTEGER AS $$
DECLARE
  r INTEGER;
BEGIN
  SELECT (i + 1 + 1.0)::INTEGER INTO r;
  RETURN r;
END; $$ LANGUAGE plpgsql;

SELECT PLUS_TWO(-1);
SELECT PLUS_TWO(2);

-- SQL function --- use LIMIT to keep it from being inlined
CREATE FUNCTION PLUS_ONE(i INTEGER) RETURNS INTEGER AS
$$ SELECT (i + 1.0)::INTEGER LIMIT 1 $$ LANGUAGE SQL;

SELECT PLUS_ONE(3);
SELECT PLUS_ONE(1);

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- immutable SQL function --- can be executed at plan time
CREATE FUNCTION PLUS_THREE(i INTEGER) RETURNS INTEGER AS
$$ SELECT i + 3 LIMIT 1 $$ IMMUTABLE LANGUAGE SQL;

SELECT PLUS_THREE(8);
SELECT PLUS_THREE(10);

SELECT toplevel, calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- pg_stat_statements.track = none
--
SET pg_stat_statements.track = 'none';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

SELECT 1 AS "one";
SELECT 1 + 1 AS "two";

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
