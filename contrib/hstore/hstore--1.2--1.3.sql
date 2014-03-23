/* contrib/hstore/hstore--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION hstore_to_jsonb(hstore)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'hstore_to_jsonb'
LANGUAGE C IMMUTABLE STRICT;

CREATE CAST (hstore AS jsonb)
  WITH FUNCTION hstore_to_jsonb(hstore);

CREATE FUNCTION hstore_to_jsonb_loose(hstore)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'hstore_to_jsonb_loose'
LANGUAGE C IMMUTABLE STRICT;
