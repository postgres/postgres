/* contrib/ltree/ltree--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ltree UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION hash_ltree(ltree)
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION hash_ltree_extended(ltree, bigint)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR CLASS hash_ltree_ops
DEFAULT FOR TYPE ltree USING hash
AS
	OPERATOR	1	= ,
	FUNCTION	1	hash_ltree(ltree),
	FUNCTION	2	hash_ltree_extended(ltree, bigint);

ALTER OPERATOR =(ltree, ltree) SET (HASHES);
