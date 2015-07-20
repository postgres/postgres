/* contrib/pg_trgm/pg_trgm--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION gin_trgm_triconsistent(internal, int2, text, int4, internal, internal, internal)
RETURNS "char"
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

ALTER OPERATOR FAMILY gin_trgm_ops USING gin ADD
        FUNCTION        6    (text, text)   gin_trgm_triconsistent (internal, int2, text, int4, internal, internal, internal);
