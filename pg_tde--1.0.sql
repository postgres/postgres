/* contrib/pg_tde/pg_tde--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_tde" to load this file. \quit

CREATE FUNCTION pg_tdeam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Access method
CREATE ACCESS METHOD pg_tde TYPE TABLE HANDLER pg_tdeam_handler;
COMMENT ON ACCESS METHOD pg_tde IS 'pg_tde table access method';

-- Opclasses

