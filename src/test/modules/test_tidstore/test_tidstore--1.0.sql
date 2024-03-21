/* src/test/modules/test_tidstore/test_tidstore--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_tidstore" to load this file. \quit

CREATE FUNCTION test_create(
shared bool)
RETURNS void STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION do_set_block_offsets(
blkno bigint,
offsets int2[])
RETURNS bigint STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION check_set_block_offsets()
RETURNS void STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_is_full()
RETURNS bool STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_destroy()
RETURNS void STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;
