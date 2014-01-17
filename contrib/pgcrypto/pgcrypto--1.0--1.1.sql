/* contrib/pgcrypto/pgcrypto--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgcrypto UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION gen_random_uuid()
RETURNS uuid
AS 'MODULE_PATHNAME', 'pg_random_uuid'
LANGUAGE C VOLATILE;
