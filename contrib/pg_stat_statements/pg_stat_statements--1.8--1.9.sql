/* contrib/pg_stat_statements/pg_stat_statements--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.9'" to load this file. \quit

--- Define pg_stat_statements_info
CREATE FUNCTION pg_stat_statements_info(
    OUT dealloc bigint,
    OUT stats_reset timestamp with time zone
)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

CREATE VIEW pg_stat_statements_info AS
  SELECT * FROM pg_stat_statements_info();

GRANT SELECT ON pg_stat_statements_info TO PUBLIC;
