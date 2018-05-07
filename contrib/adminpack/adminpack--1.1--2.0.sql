/* contrib/adminpack/adminpack--1.1--2.0.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION adminpack UPDATE TO '2.0'" to load this file. \quit

/* ***********************************************
 * Administrative functions for PostgreSQL
 * *********************************************** */

/* generic file access functions */

CREATE OR REPLACE FUNCTION pg_catalog.pg_file_write(text, text, bool)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_file_write_v1_1'
LANGUAGE C VOLATILE STRICT;

REVOKE EXECUTE ON FUNCTION pg_catalog.pg_file_write(text, text, bool) FROM PUBLIC;

CREATE OR REPLACE FUNCTION pg_catalog.pg_file_rename(text, text, text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_file_rename_v1_1'
LANGUAGE C VOLATILE;

REVOKE EXECUTE ON FUNCTION pg_catalog.pg_file_rename(text, text, text) FROM PUBLIC;

CREATE OR REPLACE FUNCTION pg_catalog.pg_file_rename(text, text)
RETURNS bool
AS 'SELECT pg_catalog.pg_file_rename($1, $2, NULL::pg_catalog.text);'
LANGUAGE SQL VOLATILE STRICT;

CREATE OR REPLACE FUNCTION pg_catalog.pg_file_unlink(text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_file_unlink_v1_1'
LANGUAGE C VOLATILE STRICT;

REVOKE EXECUTE ON FUNCTION pg_catalog.pg_file_unlink(text) FROM PUBLIC;

CREATE OR REPLACE FUNCTION pg_catalog.pg_logdir_ls()
RETURNS setof record
AS 'MODULE_PATHNAME', 'pg_logdir_ls_v1_1'
LANGUAGE C VOLATILE STRICT;

REVOKE EXECUTE ON FUNCTION pg_catalog.pg_logdir_ls() FROM PUBLIC;

/* These functions are now in the backend and callers should update to use those */

DROP FUNCTION pg_file_read(text, bigint, bigint);

DROP FUNCTION pg_file_length(text);

DROP FUNCTION pg_logfile_rotate();
