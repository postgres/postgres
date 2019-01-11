/* contrib/pg_stat_statements/pg_stat_statements--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_stat_statements UPDATE TO '1.7'" to load this file. \quit

/* First we have to remove them from the extension */
ALTER EXTENSION pg_stat_statements DROP FUNCTION pg_stat_statements_reset();

/* Then we can drop them */
DROP FUNCTION pg_stat_statements_reset();

/* Now redefine */
CREATE FUNCTION pg_stat_statements_reset(IN userid Oid DEFAULT 0,
	IN dbid Oid DEFAULT 0,
	IN queryid bigint DEFAULT 0
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_stat_statements_reset_1_7'
LANGUAGE C STRICT PARALLEL SAFE;

-- Don't want this to be available to non-superusers.
REVOKE ALL ON FUNCTION pg_stat_statements_reset(Oid, Oid, bigint) FROM PUBLIC;
