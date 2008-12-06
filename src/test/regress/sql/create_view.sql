--
-- CREATE_VIEW
-- Virtual class definitions
--	(this also tests the query rewrite system)
--

CREATE VIEW street AS
   SELECT r.name, r.thepath, c.cname AS cname 
   FROM ONLY road r, real_city c
   WHERE c.outline ## r.thepath;

CREATE VIEW iexit AS
   SELECT ih.name, ih.thepath, 
	interpt_pp(ih.thepath, r.thepath) AS exit
   FROM ihighway ih, ramp r
   WHERE ih.thepath ## r.thepath;

CREATE VIEW toyemp AS
   SELECT name, age, location, 12*salary AS annualsal
   FROM emp;

-- Test comments
COMMENT ON VIEW noview IS 'no view';
COMMENT ON VIEW toyemp IS 'is a view';
COMMENT ON VIEW toyemp IS NULL;

--
-- CREATE OR REPLACE VIEW
--

CREATE TABLE viewtest_tbl (a int, b int);
COPY viewtest_tbl FROM stdin;
5	10
10	15
15	20
20	25
\.

CREATE OR REPLACE VIEW viewtest AS
	SELECT * FROM viewtest_tbl;

CREATE OR REPLACE VIEW viewtest AS
	SELECT * FROM viewtest_tbl WHERE a > 10;

SELECT * FROM viewtest;

CREATE OR REPLACE VIEW viewtest AS
	SELECT a, b FROM viewtest_tbl WHERE a > 5 ORDER BY b DESC;

SELECT * FROM viewtest;

-- should fail
CREATE OR REPLACE VIEW viewtest AS
	SELECT a FROM viewtest_tbl WHERE a <> 20;

-- should fail
CREATE OR REPLACE VIEW viewtest AS
	SELECT 1, * FROM viewtest_tbl;

-- should fail
CREATE OR REPLACE VIEW viewtest AS
	SELECT a, b::numeric FROM viewtest_tbl;

-- should work 
CREATE OR REPLACE VIEW viewtest AS
	SELECT a, b, 0 AS c FROM viewtest_tbl;

DROP VIEW viewtest;
DROP TABLE viewtest_tbl;

-- tests for temporary views

CREATE SCHEMA temp_view_test
    CREATE TABLE base_table (a int, id int)
    CREATE TABLE base_table2 (a int, id int);

SET search_path TO temp_view_test, public;

CREATE TEMPORARY TABLE temp_table (a int, id int);

-- should be created in temp_view_test schema
CREATE VIEW v1 AS SELECT * FROM base_table;
-- should be created in temp object schema
CREATE VIEW v1_temp AS SELECT * FROM temp_table;
-- should be created in temp object schema
CREATE TEMP VIEW v2_temp AS SELECT * FROM base_table;
-- should be created in temp_views schema
CREATE VIEW temp_view_test.v2 AS SELECT * FROM base_table;
-- should fail
CREATE VIEW temp_view_test.v3_temp AS SELECT * FROM temp_table;
-- should fail
CREATE SCHEMA test_schema
    CREATE TEMP VIEW testview AS SELECT 1;

-- joins: if any of the join relations are temporary, the view
-- should also be temporary

-- should be non-temp
CREATE VIEW v3 AS
    SELECT t1.a AS t1_a, t2.a AS t2_a
    FROM base_table t1, base_table2 t2
    WHERE t1.id = t2.id;
-- should be temp (one join rel is temp)
CREATE VIEW v4_temp AS
    SELECT t1.a AS t1_a, t2.a AS t2_a
    FROM base_table t1, temp_table t2
    WHERE t1.id = t2.id;
-- should be temp
CREATE VIEW v5_temp AS
    SELECT t1.a AS t1_a, t2.a AS t2_a, t3.a AS t3_a
    FROM base_table t1, base_table2 t2, temp_table t3
    WHERE t1.id = t2.id and t2.id = t3.id;

-- subqueries
CREATE VIEW v4 AS SELECT * FROM base_table WHERE id IN (SELECT id FROM base_table2);
CREATE VIEW v5 AS SELECT t1.id, t2.a FROM base_table t1, (SELECT * FROM base_table2) t2;
CREATE VIEW v6 AS SELECT * FROM base_table WHERE EXISTS (SELECT 1 FROM base_table2);
CREATE VIEW v7 AS SELECT * FROM base_table WHERE NOT EXISTS (SELECT 1 FROM base_table2);
CREATE VIEW v8 AS SELECT * FROM base_table WHERE EXISTS (SELECT 1);

CREATE VIEW v6_temp AS SELECT * FROM base_table WHERE id IN (SELECT id FROM temp_table);
CREATE VIEW v7_temp AS SELECT t1.id, t2.a FROM base_table t1, (SELECT * FROM temp_table) t2;
CREATE VIEW v8_temp AS SELECT * FROM base_table WHERE EXISTS (SELECT 1 FROM temp_table);
CREATE VIEW v9_temp AS SELECT * FROM base_table WHERE NOT EXISTS (SELECT 1 FROM temp_table);

-- a view should also be temporary if it references a temporary view
CREATE VIEW v10_temp AS SELECT * FROM v7_temp;
CREATE VIEW v11_temp AS SELECT t1.id, t2.a FROM base_table t1, v10_temp t2;
CREATE VIEW v12_temp AS SELECT true FROM v11_temp;

-- a view should also be temporary if it references a temporary sequence
CREATE SEQUENCE seq1;
CREATE TEMPORARY SEQUENCE seq1_temp;
CREATE VIEW v9 AS SELECT seq1.is_called FROM seq1;
CREATE VIEW v13_temp AS SELECT seq1_temp.is_called FROM seq1_temp;

SELECT relname FROM pg_class
    WHERE relname LIKE 'v_'  
    AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'temp_view_test')
    ORDER BY relname;
SELECT relname FROM pg_class
    WHERE relname LIKE 'v%' 
    AND relnamespace IN (SELECT oid FROM pg_namespace WHERE nspname LIKE 'pg_temp%')
    ORDER BY relname;

CREATE SCHEMA testviewschm2;
SET search_path TO testviewschm2, public;

CREATE TABLE t1 (num int, name text);
CREATE TABLE t2 (num2 int, value text);
CREATE TEMP TABLE tt (num2 int, value text);

CREATE VIEW nontemp1 AS SELECT * FROM t1 CROSS JOIN t2;
CREATE VIEW temporal1 AS SELECT * FROM t1 CROSS JOIN tt;
CREATE VIEW nontemp2 AS SELECT * FROM t1 INNER JOIN t2 ON t1.num = t2.num2;
CREATE VIEW temporal2 AS SELECT * FROM t1 INNER JOIN tt ON t1.num = tt.num2;
CREATE VIEW nontemp3 AS SELECT * FROM t1 LEFT JOIN t2 ON t1.num = t2.num2;
CREATE VIEW temporal3 AS SELECT * FROM t1 LEFT JOIN tt ON t1.num = tt.num2;
CREATE VIEW nontemp4 AS SELECT * FROM t1 LEFT JOIN t2 ON t1.num = t2.num2 AND t2.value = 'xxx';
CREATE VIEW temporal4 AS SELECT * FROM t1 LEFT JOIN tt ON t1.num = tt.num2 AND tt.value = 'xxx';

SELECT relname FROM pg_class
    WHERE relname LIKE 'nontemp%'
    AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'testviewschm2')
    ORDER BY relname;
SELECT relname FROM pg_class
    WHERE relname LIKE 'temporal%' 
    AND relnamespace IN (SELECT oid FROM pg_namespace WHERE nspname LIKE 'pg_temp%')
    ORDER BY relname;

CREATE TABLE tbl1 ( a int, b int);
CREATE TABLE tbl2 (c int, d int);
CREATE TABLE tbl3 (e int, f int);
CREATE TABLE tbl4 (g int, h int);
CREATE TEMP TABLE tmptbl (i int, j int);

--Should be in testviewschm2
CREATE   VIEW  pubview AS SELECT * FROM tbl1 WHERE tbl1.a
BETWEEN (SELECT d FROM tbl2 WHERE c = 1) AND (SELECT e FROM tbl3 WHERE f = 2)
AND EXISTS (SELECT g FROM tbl4 LEFT JOIN tbl3 ON tbl4.h = tbl3.f);

SELECT count(*) FROM pg_class where relname = 'pubview'
AND relnamespace IN (SELECT OID FROM pg_namespace WHERE nspname = 'testviewschm2');

--Should be in temp object schema
CREATE   VIEW  mytempview AS SELECT * FROM tbl1 WHERE tbl1.a
BETWEEN (SELECT d FROM tbl2 WHERE c = 1) AND (SELECT e FROM tbl3 WHERE f = 2)
AND EXISTS (SELECT g FROM tbl4 LEFT JOIN tbl3 ON tbl4.h = tbl3.f)
AND NOT EXISTS (SELECT g FROM tbl4 LEFT JOIN tmptbl ON tbl4.h = tmptbl.j);

SELECT count(*) FROM pg_class where relname LIKE 'mytempview'
And relnamespace IN (SELECT OID FROM pg_namespace WHERE nspname LIKE 'pg_temp%');

DROP SCHEMA temp_view_test CASCADE;
DROP SCHEMA testviewschm2 CASCADE;

SET search_path to public;
