/* src/test/modules/test_dsa/test_dsa--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_dsa" to load this file. \quit

CREATE FUNCTION test_dsa_basic()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_dsa_resowners()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;
