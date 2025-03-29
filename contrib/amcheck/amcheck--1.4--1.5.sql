/* contrib/amcheck/amcheck--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.5'" to load this file. \quit


-- gin_index_check()
--
CREATE FUNCTION gin_index_check(index regclass)
RETURNS VOID
AS 'MODULE_PATHNAME', 'gin_index_check'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gin_index_check(regclass) FROM PUBLIC;
