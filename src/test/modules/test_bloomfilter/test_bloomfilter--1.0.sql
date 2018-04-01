/* src/test/modules/test_bloomfilter/test_bloomfilter--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_bloomfilter" to load this file. \quit

CREATE FUNCTION test_bloomfilter(power integer,
    nelements bigint,
    seed integer DEFAULT -1,
    tests integer DEFAULT 1)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
