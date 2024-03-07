/* src/test/modules/test_radixtree/test_radixtree--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_radixtree" to load this file. \quit

CREATE FUNCTION test_radixtree()
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
