/* contrib/dblink/dblink--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION dblink UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION dblink_fdw_validator(
    options text[],
    catalog oid
)
RETURNS void
AS 'MODULE_PATHNAME', 'dblink_fdw_validator'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER dblink_fdw VALIDATOR dblink_fdw_validator;
