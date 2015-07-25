/* contrib/tsm_system_rows/tsm_system_rows--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tsm_system_rows" to load this file. \quit

CREATE FUNCTION system_rows(internal)
RETURNS tsm_handler
AS 'MODULE_PATHNAME', 'tsm_system_rows_handler'
LANGUAGE C STRICT;
