/* contrib/postgres_fdw/postgres_fdw--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION postgres_fdw UPDATE TO '1.3'" to load this file. \quit

-- takes internal parameter to prevent calling from SQL
CREATE FUNCTION postgres_fdw_connection(oid, oid, internal)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT PARALLEL RESTRICTED;

ALTER FOREIGN DATA WRAPPER postgres_fdw CONNECTION postgres_fdw_connection;
