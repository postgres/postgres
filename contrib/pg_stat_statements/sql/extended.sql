-- Tests with extended query protocol

SET pg_stat_statements.track_utility = FALSE;

-- This test checks that an execute message sets a query ID.
SELECT query_id IS NOT NULL AS query_id_set
  FROM pg_stat_activity WHERE pid = pg_backend_pid() \bind \g

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT $1 \parse stmt1
SELECT $1, $2 \parse stmt2
SELECT $1, $2, $3 \parse stmt3
SELECT $1 \bind 'unnamed_val1' \g
\bind_named stmt1 'stmt1_val1' \g
\bind_named stmt2 'stmt2_val1' 'stmt2_val2' \g
\bind_named stmt3 'stmt3_val1' 'stmt3_val2' 'stmt3_val3' \g
\bind_named stmt3 'stmt3_val4' 'stmt3_val5' 'stmt3_val6' \g
\bind_named stmt2 'stmt2_val3' 'stmt2_val4' \g
\bind_named stmt1 'stmt1_val1' \g

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Various parameter numbering patterns
-- Unique query IDs with parameter numbers switched.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE ($1::int, 7) IN ((8, $2::int), ($3::int, 9)) \bind '1' '2' '3' \g
SELECT WHERE ($2::int, 10) IN ((11, $3::int), ($1::int, 12)) \bind '1' '2' '3' \g
SELECT WHERE $1::int IN ($2::int, $3::int) \bind '1' '2' '3' \g
SELECT WHERE $2::int IN ($3::int, $1::int) \bind '1' '2' '3' \g
SELECT WHERE $3::int IN ($1::int, $2::int) \bind '1' '2' '3' \g
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
-- Two groups of two queries with the same query ID.
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE '1'::int IN ($1::int, '2'::int) \bind '1' \g
SELECT WHERE '4'::int IN ($1::int, '5'::int) \bind '2' \g
SELECT WHERE $2::int IN ($1::int, '1'::int) \bind '1' '2' \g
SELECT WHERE $2::int IN ($1::int, '2'::int) \bind '3' '4' \g
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- no squashable list, the parameters id's are kept as-is
SELECT WHERE $3 = $1 AND $2 = $4 \bind 1 2 1 2 \g
-- squashable list, so the parameter IDs will be re-assigned
SELECT WHERE 1 IN (1, 2, 3) AND $3 = $1 AND $2 = $4 \bind 1 2 1 2 \g

SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
