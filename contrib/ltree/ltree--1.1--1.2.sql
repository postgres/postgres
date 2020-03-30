/* contrib/ltree/ltree--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ltree UPDATE TO '1.2'" to load this file. \quit

CREATE FUNCTION ltree_gist_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'ltree_gist_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION _ltree_gist_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', '_ltree_gist_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

ALTER OPERATOR FAMILY gist_ltree_ops USING gist
ADD FUNCTION 10 (ltree) ltree_gist_options (internal);

ALTER OPERATOR FAMILY gist__ltree_ops USING gist
ADD FUNCTION 10 (_ltree) _ltree_gist_options (internal);

