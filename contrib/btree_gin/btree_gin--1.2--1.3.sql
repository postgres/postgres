/* contrib/btree_gin/btree_gin--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gin UPDATE TO '1.3'" to load this file. \quit

-- uuid datatype support new in 1.3.
CREATE FUNCTION gin_extract_value_uuid(uuid, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_compare_prefix_uuid(uuid, uuid, int2, internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_extract_query_uuid(uuid, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS uuid_ops
DEFAULT FOR TYPE uuid USING gin
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       uuid_cmp(uuid,uuid),
    FUNCTION        2       gin_extract_value_uuid(uuid, internal),
    FUNCTION        3       gin_extract_query_uuid(uuid, internal, int2, internal, internal),
    FUNCTION        4       gin_btree_consistent(internal, int2, anyelement, int4, internal, internal),
    FUNCTION        5       gin_compare_prefix_uuid(uuid,uuid,int2, internal),
STORAGE         uuid;

-- name datatype support new in 1.3.
CREATE FUNCTION gin_extract_value_name(name, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_compare_prefix_name(name, name, int2, internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_extract_query_name(name, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS name_ops
DEFAULT FOR TYPE name USING gin
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btnamecmp(name,name),
    FUNCTION        2       gin_extract_value_name(name, internal),
    FUNCTION        3       gin_extract_query_name(name, internal, int2, internal, internal),
    FUNCTION        4       gin_btree_consistent(internal, int2, anyelement, int4, internal, internal),
    FUNCTION        5       gin_compare_prefix_name(name,name,int2, internal),
STORAGE         name;

-- bool datatype support new in 1.3.
CREATE FUNCTION gin_extract_value_bool(bool, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_compare_prefix_bool(bool, bool, int2, internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_extract_query_bool(bool, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS bool_ops
DEFAULT FOR TYPE bool USING gin
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       btboolcmp(bool,bool),
    FUNCTION        2       gin_extract_value_bool(bool, internal),
    FUNCTION        3       gin_extract_query_bool(bool, internal, int2, internal, internal),
    FUNCTION        4       gin_btree_consistent(internal, int2, anyelement, int4, internal, internal),
    FUNCTION        5       gin_compare_prefix_bool(bool,bool,int2, internal),
STORAGE         bool;

-- bpchar datatype support new in 1.3.
CREATE FUNCTION gin_extract_value_bpchar(bpchar, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_compare_prefix_bpchar(bpchar, bpchar, int2, internal)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION gin_extract_query_bpchar(bpchar, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR CLASS bpchar_ops
DEFAULT FOR TYPE bpchar USING gin
AS
    OPERATOR        1       <,
    OPERATOR        2       <=,
    OPERATOR        3       =,
    OPERATOR        4       >=,
    OPERATOR        5       >,
    FUNCTION        1       bpcharcmp(bpchar, bpchar),
    FUNCTION        2       gin_extract_value_bpchar(bpchar, internal),
    FUNCTION        3       gin_extract_query_bpchar(bpchar, internal, int2, internal, internal),
    FUNCTION        4       gin_btree_consistent(internal, int2, anyelement, int4, internal, internal),
    FUNCTION        5       gin_compare_prefix_bpchar(bpchar,bpchar,int2, internal),
STORAGE         bpchar;
