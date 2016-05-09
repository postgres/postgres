/* src/test/modules/test_pg_dump/test_pg_dump--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_pg_dump" to load this file. \quit

CREATE TABLE regress_pg_dump_table (
	col1 int,
	col2 int
);

GRANT SELECT ON regress_pg_dump_table TO dump_test;
GRANT SELECT(col1) ON regress_pg_dump_table TO public;

GRANT SELECT(col2) ON regress_pg_dump_table TO dump_test;
REVOKE SELECT(col2) ON regress_pg_dump_table FROM dump_test;
