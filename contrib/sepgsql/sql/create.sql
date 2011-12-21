--
-- Regression Test for Creation of Object Permission Checks
--

-- confirm required permissions using audit messages
-- @SECURITY-CONTEXT=unconfined_u:unconfined_r:unconfined_t:s0
SET sepgsql.debug_audit = true;
SET client_min_messages = LOG;

CREATE DATABASE regtest_sepgsql_test_database;

CREATE SCHEMA regtest_schema;

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

--
-- clean-up
--
DROP DATABASE IF EXISTS regtest_sepgsql_test_database;

DROP SCHEMA IF EXISTS regtest_schema CASCADE;
