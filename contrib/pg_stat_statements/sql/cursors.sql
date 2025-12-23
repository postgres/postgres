--
-- Cursors
--

-- These tests require track_utility to be enabled.
SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- DECLARE
-- SELECT is normalized.
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 1;
CLOSE cursor_stats_1;
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 2;
CLOSE cursor_stats_1;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- FETCH
BEGIN;
DECLARE cursor_stats_1 CURSOR WITH HOLD FOR SELECT 2;
DECLARE cursor_stats_2 CURSOR WITH HOLD FOR SELECT 3;
FETCH 1 IN cursor_stats_1;
FETCH 1 IN cursor_stats_2;
CLOSE cursor_stats_1;
CLOSE cursor_stats_2;
COMMIT;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- Normalization of FETCH statements
BEGIN;
DECLARE pgss_cursor CURSOR FOR SELECT FROM generate_series(1, 10);
-- implicit directions
FETCH pgss_cursor;
FETCH 1 pgss_cursor;
FETCH 2 pgss_cursor;
FETCH -1 pgss_cursor;
-- explicit NEXT
FETCH NEXT pgss_cursor;
-- explicit PRIOR
FETCH PRIOR pgss_cursor;
-- explicit FIRST
FETCH FIRST pgss_cursor;
-- explicit LAST
FETCH LAST pgss_cursor;
-- explicit ABSOLUTE
FETCH ABSOLUTE 1 pgss_cursor;
FETCH ABSOLUTE 2 pgss_cursor;
FETCH ABSOLUTE -1 pgss_cursor;
-- explicit RELATIVE
FETCH RELATIVE 1 pgss_cursor;
FETCH RELATIVE 2 pgss_cursor;
FETCH RELATIVE -1 pgss_cursor;
-- explicit FORWARD
FETCH ALL pgss_cursor;
-- explicit FORWARD ALL
FETCH FORWARD ALL pgss_cursor;
-- explicit FETCH FORWARD
FETCH FORWARD pgss_cursor;
FETCH FORWARD 1 pgss_cursor;
FETCH FORWARD 2 pgss_cursor;
FETCH FORWARD -1 pgss_cursor;
-- explicit FETCH BACKWARD
FETCH BACKWARD pgss_cursor;
FETCH BACKWARD 1 pgss_cursor;
FETCH BACKWARD 2 pgss_cursor;
FETCH BACKWARD -1 pgss_cursor;
-- explicit BACKWARD ALL
FETCH BACKWARD ALL pgss_cursor;
COMMIT;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";
