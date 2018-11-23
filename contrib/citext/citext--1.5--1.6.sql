/* contrib/citext/citext--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.6'" to load this file. \quit

CREATE FUNCTION citext_hash_extended(citext, int8)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY citext_ops USING hash ADD
    FUNCTION    2   citext_hash_extended(citext, int8);
