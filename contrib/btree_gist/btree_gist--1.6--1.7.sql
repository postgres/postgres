/* contrib/btree_gist/btree_gist--1.6--1.7.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.7'" to load this file. \quit

-- This upgrade scripts add support for bool.

-- Define the GiST support methods
CREATE FUNCTION gbt_bool_consistent(internal,bool,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_bool_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_bool_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_bool_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_bool_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_bool_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_bool_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- Create the operator class
CREATE OPERATOR CLASS gist_bool_ops
DEFAULT FOR TYPE bool USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_bool_consistent (internal, bool, int2, oid, internal),
	FUNCTION	2	gbt_bool_union (internal, internal),
	FUNCTION	3	gbt_bool_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_bool_penalty (internal, internal, internal),
	FUNCTION	6	gbt_bool_picksplit (internal, internal),
	FUNCTION	7	gbt_bool_same (gbtreekey8, gbtreekey8, internal),
	FUNCTION	9   gbt_bool_fetch (internal),
	STORAGE		gbtreekey8;
