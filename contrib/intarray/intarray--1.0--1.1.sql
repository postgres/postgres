/* contrib/intarray/intarray--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION _int_matchsel(internal, oid, internal, integer)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

ALTER OPERATOR @@ (_int4, query_int) SET (RESTRICT = _int_matchsel);
ALTER OPERATOR ~~ (query_int, _int4) SET (RESTRICT = _int_matchsel);

CREATE FUNCTION _int_overlap_sel(internal, oid, internal, integer)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION _int_contains_sel(internal, oid, internal, integer)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION _int_contained_sel(internal, oid, internal, integer)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION _int_overlap_joinsel(internal, oid, internal, smallint, internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION _int_contains_joinsel(internal, oid, internal, smallint, internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION _int_contained_joinsel(internal, oid, internal, smallint, internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

ALTER OPERATOR && (_int4, _int4) SET (RESTRICT = _int_overlap_sel, JOIN = _int_overlap_joinsel);
ALTER OPERATOR @> (_int4, _int4) SET (RESTRICT = _int_contains_sel, JOIN = _int_contains_joinsel);
ALTER OPERATOR <@ (_int4, _int4) SET (RESTRICT = _int_contained_sel, JOIN = _int_contained_joinsel);

ALTER OPERATOR @ (_int4, _int4) SET (RESTRICT = _int_contains_sel, JOIN = _int_contains_joinsel);
ALTER OPERATOR ~ (_int4, _int4) SET (RESTRICT = _int_contained_sel, JOIN = _int_contained_joinsel);
