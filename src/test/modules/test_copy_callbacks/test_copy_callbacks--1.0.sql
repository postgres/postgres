/* src/test/modules/test_copy_callbacks/test_copy_callbacks--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_copy_callbacks" to load this file. \quit

CREATE FUNCTION test_copy_to_callback(pg_catalog.regclass)
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;
