/* contrib/btree_gist/btree_gist--1.4--1.5.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.5'" to load this file. \quit

--
--
--
-- enum ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_enum_consistent(internal,anyenum,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_enum_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_enum_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_enum_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_enum_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_enum_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_enum_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- Create the operator class
CREATE OPERATOR CLASS gist_enum_ops
DEFAULT FOR TYPE anyenum USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	FUNCTION	1	gbt_enum_consistent (internal, anyenum, int2, oid, internal),
	FUNCTION	2	gbt_enum_union (internal, internal),
	FUNCTION	3	gbt_enum_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_enum_penalty (internal, internal, internal),
	FUNCTION	6	gbt_enum_picksplit (internal, internal),
	FUNCTION	7	gbt_enum_same (gbtreekey8, gbtreekey8, internal),
	STORAGE		gbtreekey8;

ALTER OPERATOR FAMILY gist_enum_ops USING gist ADD
	OPERATOR	6	<> (anyenum, anyenum) ,
	FUNCTION	9 (anyenum, anyenum) gbt_enum_fetch (internal) ;
