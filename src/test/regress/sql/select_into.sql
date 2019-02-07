--
-- SELECT_INTO
--

SELECT *
   INTO TABLE sitmp1
   FROM onek
   WHERE onek.unique1 < 2;

DROP TABLE sitmp1;

SELECT *
   INTO TABLE sitmp1
   FROM onek2
   WHERE onek2.unique1 < 2;

DROP TABLE sitmp1;

--
-- SELECT INTO and INSERT permission, if owner is not allowed to insert.
--
CREATE SCHEMA selinto_schema;
CREATE USER regress_selinto_user;
ALTER DEFAULT PRIVILEGES FOR ROLE regress_selinto_user
	  REVOKE INSERT ON TABLES FROM regress_selinto_user;
GRANT ALL ON SCHEMA selinto_schema TO public;

SET SESSION AUTHORIZATION regress_selinto_user;
SELECT * INTO TABLE selinto_schema.tmp1
	  FROM pg_class WHERE relname like '%a%';	-- Error
SELECT oid AS clsoid, relname, relnatts + 10 AS x
	  INTO selinto_schema.tmp2
	  FROM pg_class WHERE relname like '%b%';	-- Error
CREATE TABLE selinto_schema.tmp3 (a,b,c)
	   AS SELECT oid,relname,relacl FROM pg_class
	   WHERE relname like '%c%';	-- Error
RESET SESSION AUTHORIZATION;

ALTER DEFAULT PRIVILEGES FOR ROLE regress_selinto_user
	  GRANT INSERT ON TABLES TO regress_selinto_user;

SET SESSION AUTHORIZATION regress_selinto_user;
SELECT * INTO TABLE selinto_schema.tmp1
	  FROM pg_class WHERE relname like '%a%';	-- OK
SELECT oid AS clsoid, relname, relnatts + 10 AS x
	  INTO selinto_schema.tmp2
	  FROM pg_class WHERE relname like '%b%';	-- OK
CREATE TABLE selinto_schema.tmp3 (a,b,c)
	   AS SELECT oid,relname,relacl FROM pg_class
	   WHERE relname like '%c%';	-- OK
RESET SESSION AUTHORIZATION;

DROP SCHEMA selinto_schema CASCADE;
DROP USER regress_selinto_user;

-- Tests for WITH NO DATA and column name consistency
CREATE TABLE ctas_base (i int, j int);
INSERT INTO ctas_base VALUES (1, 2);
CREATE TABLE ctas_nodata (ii, jj, kk) AS SELECT i, j FROM ctas_base; -- Error
CREATE TABLE ctas_nodata (ii, jj, kk) AS SELECT i, j FROM ctas_base WITH NO DATA; -- Error
CREATE TABLE ctas_nodata (ii, jj) AS SELECT i, j FROM ctas_base; -- OK
CREATE TABLE ctas_nodata_2 (ii, jj) AS SELECT i, j FROM ctas_base WITH NO DATA; -- OK
CREATE TABLE ctas_nodata_3 (ii) AS SELECT i, j FROM ctas_base; -- OK
CREATE TABLE ctas_nodata_4 (ii) AS SELECT i, j FROM ctas_base WITH NO DATA; -- OK
SELECT * FROM ctas_nodata;
SELECT * FROM ctas_nodata_2;
SELECT * FROM ctas_nodata_3;
SELECT * FROM ctas_nodata_4;
DROP TABLE ctas_base;
DROP TABLE ctas_nodata;
DROP TABLE ctas_nodata_2;
DROP TABLE ctas_nodata_3;
DROP TABLE ctas_nodata_4;

--
-- CREATE TABLE AS/SELECT INTO as last command in a SQL function
-- have been known to cause problems
--
CREATE FUNCTION make_table() RETURNS VOID
AS $$
  CREATE TABLE created_table AS SELECT * FROM int8_tbl;
$$ LANGUAGE SQL;

SELECT make_table();

SELECT * FROM created_table;

-- Try EXPLAIN ANALYZE SELECT INTO and EXPLAIN ANALYZE CREATE TABLE AS
-- WITH NO DATA, but hide the outputs since they won't be stable.
DO $$
BEGIN
	EXECUTE 'EXPLAIN ANALYZE SELECT * INTO TABLE easi FROM int8_tbl';
	EXECUTE 'EXPLAIN ANALYZE CREATE TABLE easi2 AS SELECT * FROM int8_tbl WITH NO DATA';
END$$;

DROP TABLE created_table;
DROP TABLE easi, easi2;

--
-- Disallowed uses of SELECT ... INTO.  All should fail
--
DECLARE foo CURSOR FOR SELECT 1 INTO b;
COPY (SELECT 1 INTO frak UNION SELECT 2) TO 'blob';
SELECT * FROM (SELECT 1 INTO f) bar;
CREATE VIEW foo AS SELECT 1 INTO b;
INSERT INTO b SELECT 1 INTO f;
