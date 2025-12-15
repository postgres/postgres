/* src/test/modules/test_custom_var_stats/test_custom_var_stats--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_custom_var_stats" to load this file. \quit

CREATE FUNCTION test_custom_stats_var_create(IN name TEXT, in description TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'test_custom_stats_var_create'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_custom_stats_var_update(IN name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'test_custom_stats_var_update'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_custom_stats_var_drop(IN name TEXT)
RETURNS void
AS 'MODULE_PATHNAME', 'test_custom_stats_var_drop'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION test_custom_stats_var_report(INOUT name TEXT,
                                             OUT calls BIGINT,
                                             OUT description TEXT)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'test_custom_stats_var_report'
LANGUAGE C STRICT PARALLEL UNSAFE;
