/* contrib/adminpack/adminpack--2.0--2.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION adminpack UPDATE TO '2.1'" to load this file. \quit

/* ***********************************************
 * Administrative functions for PostgreSQL
 * *********************************************** */

/* generic file access functions */

CREATE OR REPLACE FUNCTION pg_catalog.pg_file_sync(text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_file_sync'
LANGUAGE C VOLATILE STRICT;

REVOKE EXECUTE ON FUNCTION pg_catalog.pg_file_sync(text) FROM PUBLIC;
