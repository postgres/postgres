/* src/test/modules/test_pg_db_role_setting/test_pg_db_role_setting--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_pg_db_role_setting" to load this file. \quit

CREATE FUNCTION load_test_pg_db_role_setting() RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C;
