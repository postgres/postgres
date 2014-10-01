/* contrib/pgcrypto/pgcrypto--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pgcrypto UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION armor(bytea, text[], text[])
RETURNS text
AS 'MODULE_PATHNAME', 'pg_armor'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pgp_armor_headers(text, key OUT text, value OUT text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pgp_armor_headers'
LANGUAGE C IMMUTABLE STRICT;
