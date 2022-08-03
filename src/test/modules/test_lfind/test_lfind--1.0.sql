/* src/test/modules/test_lfind/test_lfind--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_lfind" to load this file. \quit

CREATE FUNCTION test_lfind()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;
