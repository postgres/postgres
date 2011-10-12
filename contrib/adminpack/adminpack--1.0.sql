/* contrib/adminpack/adminpack--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION adminpack" to load this file. \quit

/* ***********************************************
 * Administrative functions for PostgreSQL
 * *********************************************** */

/* generic file access functions */

CREATE FUNCTION pg_catalog.pg_file_write(text, text, bool)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_file_write'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION pg_catalog.pg_file_rename(text, text, text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_file_rename'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_catalog.pg_file_rename(text, text)
RETURNS bool
AS 'SELECT pg_catalog.pg_file_rename($1, $2, NULL::pg_catalog.text);'
LANGUAGE SQL VOLATILE STRICT;

CREATE FUNCTION pg_catalog.pg_file_unlink(text)
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_file_unlink'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION pg_catalog.pg_logdir_ls()
RETURNS setof record
AS 'MODULE_PATHNAME', 'pg_logdir_ls'
LANGUAGE C VOLATILE STRICT;


/* Renaming of existing backend functions for pgAdmin compatibility */

CREATE FUNCTION pg_catalog.pg_file_read(text, bigint, bigint)
RETURNS text
AS 'pg_read_file'
LANGUAGE INTERNAL VOLATILE STRICT;

CREATE FUNCTION pg_catalog.pg_file_length(text)
RETURNS bigint
AS 'SELECT size FROM pg_catalog.pg_stat_file($1)'
LANGUAGE SQL VOLATILE STRICT;

CREATE FUNCTION pg_catalog.pg_logfile_rotate()
RETURNS int4
AS 'pg_rotate_logfile'
LANGUAGE INTERNAL VOLATILE STRICT;
