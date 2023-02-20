CREATE EXTENSION pg_stat_statements;

--
-- simple and compound statements
--
SET pg_stat_statements.track_utility = FALSE;
SET pg_stat_statements.track_planning = TRUE;
SELECT pg_stat_statements_reset();

SELECT 1 AS "int";

SELECT 'hello'
  -- multiline
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

SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- CRUD: INSERT SELECT UPDATE DELETE on test table
--
SELECT pg_stat_statements_reset();

-- utility "create table" should not be shown
CREATE TEMP TABLE test (a int, b char(20));

INSERT INTO test VALUES(generate_series(1, 10), 'aaa');
UPDATE test SET b = 'bbb' WHERE a > 7;
DELETE FROM test WHERE a > 9;

-- explicit transaction
BEGIN;
UPDATE test SET b = '111' WHERE a = 1 ;
COMMIT;

BEGIN \;
UPDATE test SET b = '222' WHERE a = 2 \;
COMMIT ;

UPDATE test SET b = '333' WHERE a = 3 \;
UPDATE test SET b = '444' WHERE a = 4 ;

BEGIN \;
UPDATE test SET b = '555' WHERE a = 5 \;
UPDATE test SET b = '666' WHERE a = 6 \;
COMMIT ;

-- many INSERT values
INSERT INTO test (a, b) VALUES (1, 'a'), (2, 'b'), (3, 'c');

-- SELECT with constants
SELECT * FROM test WHERE a > 5 ORDER BY a ;

SELECT *
  FROM test
  WHERE a > 9
  ORDER BY a ;

-- SELECT without constants
SELECT * FROM test ORDER BY a;

-- SELECT with IN clause
SELECT * FROM test WHERE a IN (1, 2, 3, 4, 5);

-- MERGE
MERGE INTO test USING test st ON (st.a = test.a AND st.a >= 4)
 WHEN MATCHED THEN UPDATE SET b = st.b || st.a::text;
MERGE INTO test USING test st ON (st.a = test.a AND st.a >= 4)
 WHEN MATCHED THEN UPDATE SET b = test.b || st.a::text;
MERGE INTO test USING test st ON (st.a = test.a AND st.a >= 4)
 WHEN MATCHED AND length(st.b) > 1 THEN UPDATE SET b = test.b || st.a::text;
MERGE INTO test USING test st ON (st.a = test.a)
 WHEN NOT MATCHED THEN INSERT (a, b) VALUES (0, NULL);
MERGE INTO test USING test st ON (st.a = test.a)
 WHEN NOT MATCHED THEN INSERT VALUES (0, NULL);	-- same as above
MERGE INTO test USING test st ON (st.a = test.a)
 WHEN NOT MATCHED THEN INSERT (b, a) VALUES (NULL, 0);
MERGE INTO test USING test st ON (st.a = test.a)
 WHEN NOT MATCHED THEN INSERT (a) VALUES (0);
MERGE INTO test USING test st ON (st.a = test.a AND st.a >= 4)
 WHEN MATCHED THEN DELETE;
MERGE INTO test USING test st ON (st.a = test.a AND st.a >= 4)
 WHEN MATCHED THEN DO NOTHING;
MERGE INTO test USING test st ON (st.a = test.a AND st.a >= 4)
 WHEN NOT MATCHED THEN DO NOTHING;

SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- INSERT, UPDATE, DELETE on test table to validate WAL generation metrics
--
SELECT pg_stat_statements_reset();

-- utility "create table" should not be shown
CREATE TABLE pgss_test (a int, b char(20));

INSERT INTO pgss_test VALUES(generate_series(1, 10), 'aaa');
UPDATE pgss_test SET b = 'bbb' WHERE a > 7;
DELETE FROM pgss_test WHERE a > 9;
-- DROP test table
DROP TABLE pgss_test;

-- Check WAL is generated for the above statements
SELECT query, calls, rows,
wal_bytes > 0 as wal_bytes_generated,
wal_records > 0 as wal_records_generated,
wal_records >= rows as wal_records_ge_rows
FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- queries with locking clauses
--
CREATE TABLE pgss_a (id integer PRIMARY KEY);
CREATE TABLE pgss_b (id integer PRIMARY KEY, a_id integer REFERENCES pgss_a);

SELECT pg_stat_statements_reset();

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
-- Track user activity and reset them
--
SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset();
CREATE ROLE regress_stats_user1;
CREATE ROLE regress_stats_user2;

SET ROLE regress_stats_user1;

SELECT 1 AS "ONE";
SELECT 1+1 AS "TWO";

RESET ROLE;
SET ROLE regress_stats_user2;

SELECT 1 AS "ONE";
SELECT 1+1 AS "TWO";

RESET ROLE;
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- Don't reset anything if any of the parameter is NULL
--
SELECT pg_stat_statements_reset(NULL);
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- remove query ('SELECT $1+$2 AS "TWO"') executed by regress_stats_user2
-- in the current_database
--
SELECT pg_stat_statements_reset(
	(SELECT r.oid FROM pg_roles AS r WHERE r.rolname = 'regress_stats_user2'),
	(SELECT d.oid FROM pg_database As d where datname = current_database()),
	(SELECT s.queryid FROM pg_stat_statements AS s
				WHERE s.query = 'SELECT $1+$2 AS "TWO"' LIMIT 1));
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- remove query ('SELECT $1 AS "ONE"') executed by two users
--
SELECT pg_stat_statements_reset(0,0,s.queryid)
	FROM pg_stat_statements AS s WHERE s.query = 'SELECT $1 AS "ONE"';
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- remove query of a user (regress_stats_user1)
--
SELECT pg_stat_statements_reset(r.oid)
		FROM pg_roles AS r WHERE r.rolname = 'regress_stats_user1';
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- reset all
--
SELECT pg_stat_statements_reset(0,0,0);
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- cleanup
--
DROP ROLE regress_stats_user1;
DROP ROLE regress_stats_user2;

--
-- access to pg_stat_statements_info view
--
SELECT pg_stat_statements_reset();
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
