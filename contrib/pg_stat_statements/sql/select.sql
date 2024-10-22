--
-- SELECT statements
--

CREATE EXTENSION pg_stat_statements;
SET pg_stat_statements.track_utility = FALSE;
SET pg_stat_statements.track_planning = TRUE;
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- simple and compound statements
--
SELECT 1 AS "int";

/* this comment should not appear in the output */
SELECT 'hello'
  -- but this one will appear
  AS "text";

SELECT 'world' AS "text";

-- transaction
BEGIN;
SELECT 1 AS "int";
SELECT 'hello' AS "text";
COMMIT;

-- compound transaction
BEGIN \;
SELECT 2.0 AS "float" \;
SELECT 'world' AS "text" \;
COMMIT;

-- compound with empty statements and spurious leading spacing
\;\;   SELECT 3 + 3 \;\;\;   SELECT ' ' || ' !' \;\;   SELECT 1 + 4 \;;

-- non ;-terminated statements
SELECT 1 + 1 + 1 AS "add" \gset
SELECT :add + 1 + 1 AS "add" \;
SELECT :add + 1 + 1 AS "add" \gset

-- set operator
SELECT 1 AS i UNION SELECT 2 ORDER BY i;

-- ? operator
select '{"a":1, "b":2}'::jsonb ? 'b';

-- cte
WITH t(f) AS (
  VALUES (1.0), (2.0)
)
  SELECT f FROM t ORDER BY f;

-- prepared statement with parameter
PREPARE pgss_test (int) AS SELECT $1, 'test' LIMIT 1;
EXECUTE pgss_test(1);
DEALLOCATE pgss_test;

SELECT calls, rows, query FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

--
-- queries with locking clauses
--
CREATE TABLE pgss_a (id integer PRIMARY KEY);
CREATE TABLE pgss_b (id integer PRIMARY KEY, a_id integer REFERENCES pgss_a);

SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- control query
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id;

-- test range tables
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_a;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_b;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_a, pgss_b; -- matches plain "FOR UPDATE"
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE OF pgss_b, pgss_a;

-- test strengths
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR NO KEY UPDATE;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR SHARE;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR KEY SHARE;

-- test wait policies
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE NOWAIT;
SELECT * FROM pgss_a JOIN pgss_b ON pgss_b.a_id = pgss_a.id FOR UPDATE SKIP LOCKED;

SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C";

DROP TABLE pgss_a, pgss_b CASCADE;

--
-- access to pg_stat_statements_info view
--
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT dealloc FROM pg_stat_statements_info;

-- FROM [ONLY]
CREATE TABLE tbl_inh(id integer);
CREATE TABLE tbl_inh_1() INHERITS (tbl_inh);
INSERT INTO tbl_inh_1 SELECT 1;

SELECT * FROM tbl_inh;
SELECT * FROM ONLY tbl_inh;

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%FROM%tbl_inh%';

-- WITH TIES
CREATE TABLE limitoption AS SELECT 0 AS val FROM generate_series(1, 10);
SELECT *
FROM limitoption
WHERE val < 2
ORDER BY val
FETCH FIRST 2 ROWS WITH TIES;

SELECT *
FROM limitoption
WHERE val < 2
ORDER BY val
FETCH FIRST 2 ROW ONLY;

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%FETCH FIRST%';

-- GROUP BY [DISTINCT]
SELECT a, b, c
FROM (VALUES (1, 2, 3), (4, NULL, 6), (7, 8, 9)) AS t (a, b, c)
GROUP BY ROLLUP(a, b), rollup(a, c)
ORDER BY a, b, c;
SELECT a, b, c
FROM (VALUES (1, 2, 3), (4, NULL, 6), (7, 8, 9)) AS t (a, b, c)
GROUP BY DISTINCT ROLLUP(a, b), rollup(a, c)
ORDER BY a, b, c;

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%GROUP BY%ROLLUP%';

-- GROUPING SET agglevelsup
SELECT (
  SELECT (
    SELECT GROUPING(a,b) FROM (VALUES (1)) v2(c)
  ) FROM (VALUES (1,2)) v1(a,b) GROUP BY (a,b)
) FROM (VALUES(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);
SELECT (
  SELECT (
    SELECT GROUPING(e,f) FROM (VALUES (1)) v2(c)
  ) FROM (VALUES (1,2)) v1(a,b) GROUP BY (a,b)
) FROM (VALUES(6,7)) v3(e,f) GROUP BY ROLLUP(e,f);

SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%SELECT GROUPING%';
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
