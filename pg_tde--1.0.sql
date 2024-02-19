/* contrib/pg_tde/pg_tde--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

CREATE FUNCTION pg_tdeam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pgtde_is_encrypted(table_name VARCHAR)
RETURNS boolean
AS $$ SELECT amname = 'pg_tde' FROM pg_class INNER JOIN pg_am ON pg_am.oid = pg_class.relam WHERE relname = table_name $$
LANGUAGE SQL;

CREATE FUNCTION pg_tde_rotate_key(key_name VARCHAR)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Access method
CREATE ACCESS METHOD pg_tde TYPE TABLE HANDLER pg_tdeam_handler;
COMMENT ON ACCESS METHOD pg_tde IS 'pg_tde table access method';

-- Opclasses

