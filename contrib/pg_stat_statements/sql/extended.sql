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
