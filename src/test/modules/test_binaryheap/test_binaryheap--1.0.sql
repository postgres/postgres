/* src/test/modules/test_binaryheap/test_binaryheap--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_binaryheap" to load this file. \quit

CREATE FUNCTION test_binaryheap() RETURNS VOID
	AS 'MODULE_PATHNAME' LANGUAGE C;
