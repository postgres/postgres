/* contrib/intarray/intarray--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION intarray" to load this file. \quit

--
-- Create the user-defined type for the 1-D integer arrays (_int4)
--

-- Query type
CREATE FUNCTION bqarr_in(cstring)
RETURNS query_int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION bqarr_out(query_int)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TYPE query_int (
	INTERNALLENGTH = -1,
	INPUT = bqarr_in,
	OUTPUT = bqarr_out
);

--only for debug
CREATE FUNCTION querytree(query_int)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;


CREATE FUNCTION boolop(_int4, query_int)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION boolop(_int4, query_int) IS 'boolean operation with array';

CREATE FUNCTION rboolop(query_int, _int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION rboolop(query_int, _int4) IS 'boolean operation with array';

CREATE FUNCTION _int_matchsel(internal, oid, internal, integer)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE OPERATOR @@ (
	LEFTARG = _int4,
	RIGHTARG = query_int,
	PROCEDURE = boolop,
	COMMUTATOR = '~~',
	RESTRICT = _int_matchsel,
	JOIN = contjoinsel
);

CREATE OPERATOR ~~ (
	LEFTARG = query_int,
	RIGHTARG = _int4,
	PROCEDURE = rboolop,
	COMMUTATOR = '@@',
	RESTRICT = _int_matchsel,
	JOIN = contjoinsel
);


--
-- External C-functions for R-tree methods
--

-- Comparison methods

CREATE FUNCTION _int_contains(_int4, _int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION _int_contains(_int4, _int4) IS 'contains';

CREATE FUNCTION _int_contained(_int4, _int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION _int_contained(_int4, _int4) IS 'contained in';

CREATE FUNCTION _int_overlap(_int4, _int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION _int_overlap(_int4, _int4) IS 'overlaps';

CREATE FUNCTION _int_same(_int4, _int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION _int_same(_int4, _int4) IS 'same as';

CREATE FUNCTION _int_different(_int4, _int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION _int_different(_int4, _int4) IS 'different';

-- support routines for indexing

CREATE FUNCTION _int_union(_int4, _int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION _int_inter(_int4, _int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

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

--
-- OPERATORS
--

CREATE OPERATOR && (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	PROCEDURE = _int_overlap,
	COMMUTATOR = '&&',
	RESTRICT = _int_overlap_sel,
	JOIN = _int_overlap_joinsel
);

--CREATE OPERATOR = (
--	LEFTARG = _int4,
--	RIGHTARG = _int4,
--	PROCEDURE = _int_same,
--	COMMUTATOR = '=',
--	NEGATOR = '<>',
--	RESTRICT = eqsel,
--	JOIN = eqjoinsel,
--	SORT1 = '<',
--	SORT2 = '<'
--);

--CREATE OPERATOR <> (
--	LEFTARG = _int4,
--	RIGHTARG = _int4,
--	PROCEDURE = _int_different,
--	COMMUTATOR = '<>',
--	NEGATOR = '=',
--	RESTRICT = neqsel,
--	JOIN = neqjoinsel
--);

CREATE OPERATOR @> (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	PROCEDURE = _int_contains,
	COMMUTATOR = '<@',
	RESTRICT = _int_contains_sel,
	JOIN = _int_contains_joinsel
);

CREATE OPERATOR <@ (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	PROCEDURE = _int_contained,
	COMMUTATOR = '@>',
	RESTRICT = _int_contained_sel,
	JOIN = _int_contained_joinsel
);

-- obsolete:
CREATE OPERATOR @ (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	PROCEDURE = _int_contains,
	COMMUTATOR = '~',
	RESTRICT = _int_contains_sel,
	JOIN = _int_contains_joinsel
);

CREATE OPERATOR ~ (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	PROCEDURE = _int_contained,
	COMMUTATOR = '@',
	RESTRICT = _int_contained_sel,
	JOIN = _int_contained_joinsel
);

--------------
CREATE FUNCTION intset(int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION icount(_int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR # (
	RIGHTARG = _int4,
	PROCEDURE = icount
);

CREATE FUNCTION sort(_int4, text)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION sort(_int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION sort_asc(_int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION sort_desc(_int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION uniq(_int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION idx(_int4, int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR # (
	LEFTARG = _int4,
	RIGHTARG = int4,
	PROCEDURE = idx
);

CREATE FUNCTION subarray(_int4, int4, int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION subarray(_int4, int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION intarray_push_elem(_int4, int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR + (
	LEFTARG = _int4,
	RIGHTARG = int4,
	PROCEDURE = intarray_push_elem
);

CREATE FUNCTION intarray_push_array(_int4, _int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR + (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	COMMUTATOR = +,
	PROCEDURE = intarray_push_array
);

CREATE FUNCTION intarray_del_elem(_int4, int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR - (
	LEFTARG = _int4,
	RIGHTARG = int4,
	PROCEDURE = intarray_del_elem
);

CREATE FUNCTION intset_union_elem(_int4, int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR | (
	LEFTARG = _int4,
	RIGHTARG = int4,
	PROCEDURE = intset_union_elem
);

CREATE OPERATOR | (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	COMMUTATOR = |,
	PROCEDURE = _int_union
);

CREATE FUNCTION intset_subtract(_int4, _int4)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR - (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	PROCEDURE = intset_subtract
);

CREATE OPERATOR & (
	LEFTARG = _int4,
	RIGHTARG = _int4,
	COMMUTATOR = &,
	PROCEDURE = _int_inter
);
--------------

-- define the GiST support methods
CREATE FUNCTION g_int_consistent(internal,_int4,smallint,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_int_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_int_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_int_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_int_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_int_union(internal, internal)
RETURNS _int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_int_same(_int4, _int4, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;


-- Create the operator class for indexing

CREATE OPERATOR CLASS gist__int_ops
DEFAULT FOR TYPE _int4 USING gist AS
	OPERATOR	3	&&,
	OPERATOR	6	= (anyarray, anyarray),
	OPERATOR	7	@>,
	OPERATOR	8	<@,
	OPERATOR	13	@,
	OPERATOR	14	~,
	OPERATOR	20	@@ (_int4, query_int),
	FUNCTION	1	g_int_consistent (internal, _int4, smallint, oid, internal),
	FUNCTION	2	g_int_union (internal, internal),
	FUNCTION	3	g_int_compress (internal),
	FUNCTION	4	g_int_decompress (internal),
	FUNCTION	5	g_int_penalty (internal, internal, internal),
	FUNCTION	6	g_int_picksplit (internal, internal),
	FUNCTION	7	g_int_same (_int4, _int4, internal);


---------------------------------------------
-- intbig
---------------------------------------------
-- define the GiST support methods

CREATE FUNCTION _intbig_in(cstring)
RETURNS intbig_gkey
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION _intbig_out(intbig_gkey)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TYPE intbig_gkey (
        INTERNALLENGTH = -1,
        INPUT = _intbig_in,
        OUTPUT = _intbig_out
);

CREATE FUNCTION g_intbig_consistent(internal,_int4,smallint,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_intbig_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_intbig_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_intbig_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_intbig_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_intbig_union(internal, internal)
RETURNS intbig_gkey
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION g_intbig_same(intbig_gkey, intbig_gkey, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- register the opclass for indexing (not as default)

CREATE OPERATOR CLASS gist__intbig_ops
FOR TYPE _int4 USING gist
AS
	OPERATOR	3	&&,
	OPERATOR	6	= (anyarray, anyarray),
	OPERATOR	7	@>,
	OPERATOR	8	<@,
	OPERATOR	13	@,
	OPERATOR	14	~,
	OPERATOR	20	@@ (_int4, query_int),
	FUNCTION	1	g_intbig_consistent (internal, _int4, smallint, oid, internal),
	FUNCTION	2	g_intbig_union (internal, internal),
	FUNCTION	3	g_intbig_compress (internal),
	FUNCTION	4	g_intbig_decompress (internal),
	FUNCTION	5	g_intbig_penalty (internal, internal, internal),
	FUNCTION	6	g_intbig_picksplit (internal, internal),
	FUNCTION	7	g_intbig_same (intbig_gkey, intbig_gkey, internal),
	STORAGE		intbig_gkey;

--GIN

CREATE FUNCTION ginint4_queryextract(_int4, internal, int2, internal, internal, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION ginint4_consistent(internal, int2, _int4, int4, internal, internal, internal, internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS gin__int_ops
FOR TYPE _int4 USING gin
AS
	OPERATOR	3	&&,
	OPERATOR	6	= (anyarray, anyarray),
	OPERATOR	7	@>,
	OPERATOR	8	<@,
	OPERATOR	13	@,
	OPERATOR	14	~,
	OPERATOR	20	@@ (_int4, query_int),
	FUNCTION	1	btint4cmp (int4, int4),
	FUNCTION	2	ginarrayextract (anyarray, internal, internal),
	FUNCTION	3	ginint4_queryextract (_int4, internal, int2, internal, internal, internal, internal),
	FUNCTION	4	ginint4_consistent (internal, int2, _int4, int4, internal, internal, internal, internal),
	STORAGE		int4;
