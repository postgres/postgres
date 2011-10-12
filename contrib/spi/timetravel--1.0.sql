/* contrib/spi/timetravel--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION timetravel" to load this file. \quit

CREATE FUNCTION timetravel()
RETURNS trigger
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION set_timetravel(name, int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;

CREATE FUNCTION get_timetravel(name)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT;
