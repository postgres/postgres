/* contrib/fuzzystrmatch/fuzzystrmatch--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION fuzzystrmatch UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION daitch_mokotoff(text) RETURNS text[]
AS 'MODULE_PATHNAME', 'daitch_mokotoff'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
