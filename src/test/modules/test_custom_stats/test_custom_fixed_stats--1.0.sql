/* src/test/modules/test_custom_stats/test_custom_fixed_stats--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_custom_fixed_stats" to load this file. \quit

CREATE FUNCTION test_custom_stats_fixed_update()
RETURNS void
AS 'MODULE_PATHNAME', 'test_custom_stats_fixed_update'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_custom_stats_fixed_report(OUT numcalls bigint,
    OUT stats_reset timestamptz)
RETURNS record
AS 'MODULE_PATHNAME', 'test_custom_stats_fixed_report'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_custom_stats_fixed_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'test_custom_stats_fixed_reset'
LANGUAGE C STRICT PARALLEL UNSAFE;
