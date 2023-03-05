/* contrib/pg_buffercache/pg_buffercache--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.4'" to load this file. \quit

CREATE FUNCTION pg_buffercache_summary(
    OUT buffers_used int4,
    OUT buffers_unused int4,
    OUT buffers_dirty int4,
    OUT buffers_pinned int4,
    OUT usagecount_avg float8)
AS 'MODULE_PATHNAME', 'pg_buffercache_summary'
LANGUAGE C PARALLEL SAFE;

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION pg_buffercache_summary() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_buffercache_summary() TO pg_monitor;
