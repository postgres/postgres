/* src/test/modules/test_cplusplusext/test_cplusplusext--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_cplusplusext" to load this file. \quit

CREATE FUNCTION test_cplusplus_add(int4, int4) RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
