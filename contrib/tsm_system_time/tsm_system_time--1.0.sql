/* contrib/tsm_system_time/tsm_system_time--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tsm_system_time" to load this file. \quit

CREATE FUNCTION system_time(internal)
RETURNS tsm_handler
AS 'MODULE_PATHNAME', 'tsm_system_time_handler'
LANGUAGE C STRICT;
