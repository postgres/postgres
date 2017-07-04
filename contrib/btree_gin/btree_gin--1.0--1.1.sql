/* contrib/btree_gin/btree_gin--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gin UPDATE TO '1.1'" to load this file. \quit

-- macaddr8 datatype support new in 10.0.
CREATE FUNCTION gin_extract_value_macaddr8(macaddr8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_compare_prefix_macaddr8(macaddr8, macaddr8, int2, internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_extract_query_macaddr8(macaddr8, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS macaddr8_ops
DEFAULT FOR TYPE macaddr8 USING gin
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       macaddr8_cmp(macaddr8, macaddr8),
    FUNCTION        2       gin_extract_value_macaddr8(macaddr8, internal),
    FUNCTION        3       gin_extract_query_macaddr8(macaddr8, internal, int2, internal, internal),
    FUNCTION        4       gin_btree_consistent(internal, int2, anyelement, int4, internal, internal),
    FUNCTION        5       gin_compare_prefix_macaddr8(macaddr8, macaddr8, int2, internal),
STORAGE         macaddr8;
