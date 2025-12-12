/* src/test/modules/test_toast_ext/test_toast_ext--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_toast_ext" to load this file. \quit

CREATE FUNCTION test_toast_structure_sizes()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION test_toast_flag_validation()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION test_toast_compression_ids()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
