/* contrib/pg_stat_statements/pg_stat_statements--1.11--1.12.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.12'" to load this file. \quit

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_statements DROP VIEW pg_stat_statements;
ALTER EXTENSION pg_stat_statements DROP FUNCTION pg_stat_statements(boolean);

/* Then we can drop them */
DROP VIEW pg_stat_statements;
DROP FUNCTION pg_stat_statements(boolean);

/* Now redefine */
CREATE FUNCTION pg_stat_statements(IN showtext boolean,
    OUT userid oid,
    OUT dbid oid,
    OUT toplevel bool,
    OUT queryid bigint,
    OUT query text,
    OUT plans int8,
    OUT total_plan_time float8,
    OUT min_plan_time float8,
    OUT max_plan_time float8,
    OUT mean_plan_time float8,
    OUT stddev_plan_time float8,
    OUT calls int8,
    OUT total_exec_time float8,
    OUT min_exec_time float8,
    OUT max_exec_time float8,
    OUT mean_exec_time float8,
    OUT stddev_exec_time float8,
    OUT rows int8,
    OUT shared_blks_hit int8,
    OUT shared_blks_read int8,
    OUT shared_blks_dirtied int8,
    OUT shared_blks_written int8,
    OUT local_blks_hit int8,
    OUT local_blks_read int8,
    OUT local_blks_dirtied int8,
    OUT local_blks_written int8,
    OUT temp_blks_read int8,
    OUT temp_blks_written int8,
    OUT shared_blk_read_time float8,
    OUT shared_blk_write_time float8,
    OUT local_blk_read_time float8,
    OUT local_blk_write_time float8,
    OUT temp_blk_read_time float8,
    OUT temp_blk_write_time float8,
    OUT wal_records int8,
    OUT wal_fpi int8,
    OUT wal_bytes numeric,
    OUT jit_functions int8,
    OUT jit_generation_time float8,
    OUT jit_inlining_count int8,
    OUT jit_inlining_time float8,
    OUT jit_optimization_count int8,
    OUT jit_optimization_time float8,
    OUT jit_emission_count int8,
    OUT jit_emission_time float8,
    OUT jit_deform_count int8,
    OUT jit_deform_time float8,
    OUT parallel_workers_to_launch int8,
    OUT parallel_workers_launched int8,
    OUT stats_since timestamp with time zone,
    OUT minmax_stats_since timestamp with time zone
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stat_statements_1_12'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statements AS
  SELECT * FROM pg_stat_statements(true);

GRANT SELECT ON pg_stat_statements TO PUBLIC;
