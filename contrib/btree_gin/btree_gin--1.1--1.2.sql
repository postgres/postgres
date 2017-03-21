/* contrib/btree_gin/btree_gin--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gin UPDATE TO '1.1'" to load this file. \quit

--
--
--
-- enum ops
--
--


CREATE FUNCTION gin_extract_value_anyenum(anyenum, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_compare_prefix_anyenum(anyenum, anyenum, int2, internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_extract_query_anyenum(anyenum, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_enum_cmp(anyenum, anyenum)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS enum_ops
DEFAULT FOR TYPE anyenum USING gin
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       gin_enum_cmp(anyenum,anyenum),
    FUNCTION        2       gin_extract_value_anyenum(anyenum, internal),
    FUNCTION        3       gin_extract_query_anyenum(anyenum, internal, int2, internal, internal),
    FUNCTION        4       gin_btree_consistent(internal, int2, anyelement, int4, internal, internal),
    FUNCTION        5       gin_compare_prefix_anyenum(anyenum,anyenum,int2, internal),
STORAGE         anyenum;
