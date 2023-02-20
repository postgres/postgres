--
-- Utility commands
--

-- These tests require track_utility to be enabled.
SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset();

SELECT 1;
CREATE TEMP TABLE stats_util_tab (a int, b char(20));
CREATE INDEX test_b ON stats_util_tab(b);
DROP TABLE stats_util_tab \;
DROP TABLE IF EXISTS stats_util_tab;
-- This DROP query uses two different strings, still they count as one entry.
DROP TABLE IF EXISTS stats_util_tab \;
Drop Table If Exists stats_util_tab;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset();

--
-- Track the total number of rows retrieved or affected by the utility
-- commands of COPY, FETCH, CREATE TABLE AS, CREATE MATERIALIZED VIEW,
-- REFRESH MATERIALIZED VIEW and SELECT INTO
--
CREATE TABLE pgss_ctas AS SELECT a, 'ctas' b FROM generate_series(1, 10) a;
SELECT generate_series(1, 10) c INTO pgss_select_into;
COPY pgss_ctas (a, b) FROM STDIN;
11	copy
12	copy
13	copy
\.
CREATE MATERIALIZED VIEW pgss_matv AS SELECT * FROM pgss_ctas;
REFRESH MATERIALIZED VIEW pgss_matv;
BEGIN;
DECLARE pgss_cursor CURSOR FOR SELECT * FROM pgss_matv;
FETCH NEXT pgss_cursor;
FETCH FORWARD 5 pgss_cursor;
FETCH FORWARD ALL pgss_cursor;
COMMIT;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

DROP MATERIALIZED VIEW pgss_matv;
DROP TABLE pgss_ctas;
DROP TABLE pgss_select_into;

SELECT pg_stat_statements_reset();

-- SET statements.
-- These use two different strings, still they count as one entry.
SET work_mem = '1MB';
Set work_mem = '1MB';
SET work_mem = '2MB';
RESET work_mem;
SET enable_seqscan = off;
SET enable_seqscan = on;
RESET enable_seqscan;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
