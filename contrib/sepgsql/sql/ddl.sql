--
-- Regression Test for DDL of Object Permission Checks
--

-- clean-up in case a prior regression run failed
SET client_min_messages TO 'warning';
DROP DATABASE IF EXISTS sepgsql_test_regression;
DROP USER IF EXISTS regress_sepgsql_test_user;
RESET client_min_messages;

-- confirm required permissions using audit messages
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:sepgsql_regtest_superuser_t:s0
SET sepgsql.debug_audit = true;
SET client_min_messages = LOG;

--
-- CREATE Permission checks
--
CREATE DATABASE sepgsql_test_regression;

CREATE USER regress_sepgsql_test_user;

CREATE SCHEMA regtest_schema;

GRANT ALL ON SCHEMA regtest_schema TO regress_sepgsql_test_user;

SET search_path = regtest_schema, public;

CREATE TABLE regtest_table (x serial primary key, y text);

ALTER TABLE regtest_table ADD COLUMN z int;

CREATE TABLE regtest_table_2 (a int) WITH OIDS;

-- corresponding toast table should not have label and permission checks
ALTER TABLE regtest_table_2 ADD COLUMN b text;

-- VACUUM FULL internally create a new table and swap them later.
VACUUM FULL regtest_table;

CREATE VIEW regtest_view AS SELECT * FROM regtest_table WHERE x < 100;

CREATE SEQUENCE regtest_seq;

CREATE TYPE regtest_comptype AS (a int, b text);

CREATE FUNCTION regtest_func(text,int[]) RETURNS bool LANGUAGE plpgsql
	   AS 'BEGIN RAISE NOTICE ''regtest_func => %'', $1; RETURN true; END';

CREATE AGGREGATE regtest_agg (
           sfunc1 = int4pl, basetype = int4, stype1 = int4, initcond1 = '0'
);

-- CREATE objects owned by others
SET SESSION AUTHORIZATION regress_sepgsql_test_user;

SET search_path = regtest_schema, public;

CREATE TABLE regtest_table_3 (x int, y serial);

CREATE VIEW regtest_view_2 AS SELECT * FROM regtest_table_3 WHERE x < y;

CREATE FUNCTION regtest_func_2(int) RETURNS bool LANGUAGE plpgsql
           AS 'BEGIN RETURN $1 * $1 < 100; END';

RESET SESSION AUTHORIZATION;

--
-- ALTER and CREATE/DROP extra attribute permissions
--
CREATE TABLE regtest_table_4 (x int primary key, y int, z int);
CREATE INDEX regtest_index_tbl4_y ON regtest_table_4(y);
CREATE INDEX regtest_index_tbl4_z ON regtest_table_4(z);
ALTER TABLE regtest_table_4 ALTER COLUMN y TYPE float;
DROP INDEX regtest_index_tbl4_y;
ALTER TABLE regtest_table_4
      ADD CONSTRAINT regtest_tbl4_con EXCLUDE USING btree (z WITH =);
DROP TABLE regtest_table_4 CASCADE;

--
-- DROP Permission checks (with clean-up)
--

DROP FUNCTION regtest_func(text,int[]);
DROP AGGREGATE regtest_agg(int);

DROP SEQUENCE regtest_seq;
DROP VIEW regtest_view;

ALTER TABLE regtest_table DROP COLUMN y;
ALTER TABLE regtest_table_2 SET WITHOUT OIDS;

DROP TABLE regtest_table;

DROP OWNED BY regress_sepgsql_test_user;

DROP DATABASE sepgsql_test_regression;
DROP USER regress_sepgsql_test_user;
DROP SCHEMA IF EXISTS regtest_schema CASCADE;
