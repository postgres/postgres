/* contrib/pg_prewarm/pg_prewarm--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_prewarm" to load this file. \quit

-- Register the function.
CREATE FUNCTION pg_prewarm(regclass,
						   mode text default 'buffer',
						   fork text default 'main',
						   first_block int8 default null,
						   last_block int8 default null)
RETURNS int8
AS 'MODULE_PATHNAME', 'pg_prewarm'
LANGUAGE C;
