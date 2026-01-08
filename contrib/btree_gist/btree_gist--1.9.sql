/* contrib/btree_gist/btree_gist--1.9.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION btree_gist" to load this file. \quit

CREATE FUNCTION gbtreekey2_in(cstring)
RETURNS gbtreekey2
AS 'MODULE_PATHNAME', 'gbtreekey_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbtreekey2_out(gbtreekey2)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gbtreekey_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE gbtreekey2 (
	INTERNALLENGTH = 2,
	INPUT  = gbtreekey2_in,
	OUTPUT = gbtreekey2_out
);

CREATE FUNCTION gbtreekey4_in(cstring)
RETURNS gbtreekey4
AS 'MODULE_PATHNAME', 'gbtreekey_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbtreekey4_out(gbtreekey4)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gbtreekey_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE gbtreekey4 (
	INTERNALLENGTH = 4,
	INPUT  = gbtreekey4_in,
	OUTPUT = gbtreekey4_out
);

CREATE FUNCTION gbtreekey8_in(cstring)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME', 'gbtreekey_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbtreekey8_out(gbtreekey8)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gbtreekey_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE gbtreekey8 (
	INTERNALLENGTH = 8,
	INPUT  = gbtreekey8_in,
	OUTPUT = gbtreekey8_out
);

CREATE FUNCTION gbtreekey16_in(cstring)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME', 'gbtreekey_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbtreekey16_out(gbtreekey16)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gbtreekey_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE gbtreekey16 (
	INTERNALLENGTH = 16,
	INPUT  = gbtreekey16_in,
	OUTPUT = gbtreekey16_out
);

CREATE FUNCTION gbtreekey32_in(cstring)
RETURNS gbtreekey32
AS 'MODULE_PATHNAME', 'gbtreekey_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbtreekey32_out(gbtreekey32)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gbtreekey_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE gbtreekey32 (
	INTERNALLENGTH = 32,
	INPUT  = gbtreekey32_in,
	OUTPUT = gbtreekey32_out
);

CREATE FUNCTION gbtreekey_var_in(cstring)
RETURNS gbtreekey_var
AS 'MODULE_PATHNAME', 'gbtreekey_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbtreekey_var_out(gbtreekey_var)
RETURNS cstring
AS 'MODULE_PATHNAME', 'gbtreekey_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE gbtreekey_var (
	INTERNALLENGTH = VARIABLE,
	INPUT  = gbtreekey_var_in,
	OUTPUT = gbtreekey_var_out,
	STORAGE = EXTENDED
);

--common support functions

CREATE FUNCTION gist_translate_cmptype_btree(int)
RETURNS smallint
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--distance operators

CREATE FUNCTION cash_dist(money, money)
RETURNS money
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = money,
	RIGHTARG = money,
	PROCEDURE = cash_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION date_dist(date, date)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = date,
	RIGHTARG = date,
	PROCEDURE = date_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION float4_dist(float4, float4)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = float4,
	RIGHTARG = float4,
	PROCEDURE = float4_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION float8_dist(float8, float8)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = float8,
	RIGHTARG = float8,
	PROCEDURE = float8_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION int2_dist(int2, int2)
RETURNS int2
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = int2,
	RIGHTARG = int2,
	PROCEDURE = int2_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION int4_dist(int4, int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = int4,
	RIGHTARG = int4,
	PROCEDURE = int4_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION int8_dist(int8, int8)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = int8,
	RIGHTARG = int8,
	PROCEDURE = int8_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION interval_dist(interval, interval)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = interval,
	RIGHTARG = interval,
	PROCEDURE = interval_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION oid_dist(oid, oid)
RETURNS oid
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = oid,
	RIGHTARG = oid,
	PROCEDURE = oid_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION time_dist(time, time)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = time,
	RIGHTARG = time,
	PROCEDURE = time_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION ts_dist(timestamp, timestamp)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = timestamp,
	RIGHTARG = timestamp,
	PROCEDURE = ts_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION tstz_dist(timestamptz, timestamptz)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <-> (
	LEFTARG = timestamptz,
	RIGHTARG = timestamptz,
	PROCEDURE = tstz_dist,
	COMMUTATOR = '<->'
);


--
--
--
-- oid ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_oid_consistent(internal,oid,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_distance(internal,oid,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_var_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_var_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_oid_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_oid_ops
DEFAULT FOR TYPE oid USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.oid_ops ,
	FUNCTION	1	gbt_oid_consistent (internal, oid, int2, oid, internal),
	FUNCTION	2	gbt_oid_union (internal, internal),
	FUNCTION	3	gbt_oid_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_oid_penalty (internal, internal, internal),
	FUNCTION	6	gbt_oid_picksplit (internal, internal),
	FUNCTION	7	gbt_oid_same (gbtreekey8, gbtreekey8, internal),
	FUNCTION	8	gbt_oid_distance (internal, oid, int2, oid, internal),
	FUNCTION	9	gbt_oid_fetch (internal),
	FUNCTION	11	gbt_oid_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey8;


--
--
--
-- int2 ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_int2_consistent(internal,int2,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_distance(internal,int2,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_union(internal, internal)
RETURNS gbtreekey4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_same(gbtreekey4, gbtreekey4, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int2_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_int2_ops
DEFAULT FOR TYPE int2 USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	1	gbt_int2_consistent (internal, int2, int2, oid, internal),
	FUNCTION	2	gbt_int2_union (internal, internal),
	FUNCTION	3	gbt_int2_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_int2_penalty (internal, internal, internal),
	FUNCTION	6	gbt_int2_picksplit (internal, internal),
	FUNCTION	7	gbt_int2_same (gbtreekey4, gbtreekey4, internal),
	FUNCTION	8	gbt_int2_distance (internal, int2, int2, oid, internal),
	FUNCTION	9	gbt_int2_fetch (internal),
	FUNCTION	11	gbt_int2_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey4;


--
--
--
-- int4 ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_int4_consistent(internal,int4,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_distance(internal,int4,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int4_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_int4_ops
DEFAULT FOR TYPE int4 USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	1	gbt_int4_consistent (internal, int4, int2, oid, internal),
	FUNCTION	2	gbt_int4_union (internal, internal),
	FUNCTION	3	gbt_int4_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_int4_penalty (internal, internal, internal),
	FUNCTION	6	gbt_int4_picksplit (internal, internal),
	FUNCTION	7	gbt_int4_same (gbtreekey8, gbtreekey8, internal),
	FUNCTION	8	gbt_int4_distance (internal, int4, int2, oid, internal),
	FUNCTION	9	gbt_int4_fetch (internal),
	FUNCTION	11	gbt_int4_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey8;


--
--
--
-- int8 ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_int8_consistent(internal,int8,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_distance(internal,int8,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_int8_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_int8_ops
DEFAULT FOR TYPE int8 USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	1	gbt_int8_consistent (internal, int8, int2, oid, internal),
	FUNCTION	2	gbt_int8_union (internal, internal),
	FUNCTION	3	gbt_int8_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_int8_penalty (internal, internal, internal),
	FUNCTION	6	gbt_int8_picksplit (internal, internal),
	FUNCTION	7	gbt_int8_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	8	gbt_int8_distance (internal, int8, int2, oid, internal),
	FUNCTION	9	gbt_int8_fetch (internal),
	FUNCTION	11	gbt_int8_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- float4 ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_float4_consistent(internal,float4,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_distance(internal,float4,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float4_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_float4_ops
DEFAULT FOR TYPE float4 USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.float_ops ,
	FUNCTION	1	gbt_float4_consistent (internal, float4, int2, oid, internal),
	FUNCTION	2	gbt_float4_union (internal, internal),
	FUNCTION	3	gbt_float4_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_float4_penalty (internal, internal, internal),
	FUNCTION	6	gbt_float4_picksplit (internal, internal),
	FUNCTION	7	gbt_float4_same (gbtreekey8, gbtreekey8, internal),
	FUNCTION	8	gbt_float4_distance (internal, float4, int2, oid, internal),
	FUNCTION	9	gbt_float4_fetch (internal),
	FUNCTION	11	gbt_float4_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey8;


--
--
--
-- float8 ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_float8_consistent(internal,float8,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_distance(internal,float8,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_float8_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_float8_ops
DEFAULT FOR TYPE float8 USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.float_ops ,
	FUNCTION	1	gbt_float8_consistent (internal, float8, int2, oid, internal),
	FUNCTION	2	gbt_float8_union (internal, internal),
	FUNCTION	3	gbt_float8_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_float8_penalty (internal, internal, internal),
	FUNCTION	6	gbt_float8_picksplit (internal, internal),
	FUNCTION	7	gbt_float8_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	8	gbt_float8_distance (internal, float8, int2, oid, internal),
	FUNCTION	9	gbt_float8_fetch (internal),
	FUNCTION	11	gbt_float8_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- timestamp ops
--
--
--

CREATE FUNCTION gbt_ts_consistent(internal,timestamp,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_distance(internal,timestamp,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_tstz_consistent(internal,timestamptz,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_tstz_distance(internal,timestamptz,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_tstz_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_ts_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_timestamp_ops
DEFAULT FOR TYPE timestamp USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	1	gbt_ts_consistent (internal, timestamp, int2, oid, internal),
	FUNCTION	2	gbt_ts_union (internal, internal),
	FUNCTION	3	gbt_ts_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_ts_penalty (internal, internal, internal),
	FUNCTION	6	gbt_ts_picksplit (internal, internal),
	FUNCTION	7	gbt_ts_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	8	gbt_ts_distance (internal, timestamp, int2, oid, internal),
	FUNCTION	9	gbt_ts_fetch (internal),
	FUNCTION	11	gbt_ts_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;

-- Create the operator class
CREATE OPERATOR CLASS gist_timestamptz_ops
DEFAULT FOR TYPE timestamptz USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	1	gbt_tstz_consistent (internal, timestamptz, int2, oid, internal),
	FUNCTION	2	gbt_ts_union (internal, internal),
	FUNCTION	3	gbt_tstz_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_ts_penalty (internal, internal, internal),
	FUNCTION	6	gbt_ts_picksplit (internal, internal),
	FUNCTION	7	gbt_ts_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	8	gbt_tstz_distance (internal, timestamptz, int2, oid, internal),
	FUNCTION	9	gbt_ts_fetch (internal),
	FUNCTION	11	gbt_ts_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- time ops
--
--
--

CREATE FUNCTION gbt_time_consistent(internal,time,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_distance(internal,time,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_timetz_consistent(internal,timetz,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_timetz_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_time_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_time_ops
DEFAULT FOR TYPE time USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	1	gbt_time_consistent (internal, time, int2, oid, internal),
	FUNCTION	2	gbt_time_union (internal, internal),
	FUNCTION	3	gbt_time_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_time_penalty (internal, internal, internal),
	FUNCTION	6	gbt_time_picksplit (internal, internal),
	FUNCTION	7	gbt_time_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	8	gbt_time_distance (internal, time, int2, oid, internal),
	FUNCTION	9	gbt_time_fetch (internal),
	FUNCTION	11	gbt_time_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;

CREATE OPERATOR CLASS gist_timetz_ops
DEFAULT FOR TYPE timetz USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_timetz_consistent (internal, timetz, int2, oid, internal),
	FUNCTION	2	gbt_time_union (internal, internal),
	FUNCTION	3	gbt_timetz_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_time_penalty (internal, internal, internal),
	FUNCTION	6	gbt_time_picksplit (internal, internal),
	FUNCTION	7	gbt_time_same (gbtreekey16, gbtreekey16, internal),
	-- no 'fetch' function, as the compress function is lossy.
	FUNCTION	11	gbt_time_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- date ops
--
--
--

CREATE FUNCTION gbt_date_consistent(internal,date,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_distance(internal,date,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_date_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_date_ops
DEFAULT FOR TYPE date USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	1	gbt_date_consistent (internal, date, int2, oid, internal),
	FUNCTION	2	gbt_date_union (internal, internal),
	FUNCTION	3	gbt_date_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_date_penalty (internal, internal, internal),
	FUNCTION	6	gbt_date_picksplit (internal, internal),
	FUNCTION	7	gbt_date_same (gbtreekey8, gbtreekey8, internal),
	FUNCTION	8	gbt_date_distance (internal, date, int2, oid, internal),
	FUNCTION	9	gbt_date_fetch (internal),
	FUNCTION	11	gbt_date_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey8;


--
--
--
-- interval ops
--
--
--

CREATE FUNCTION gbt_intv_consistent(internal,interval,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_distance(internal,interval,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_decompress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_union(internal, internal)
RETURNS gbtreekey32
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_same(gbtreekey32, gbtreekey32, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_intv_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_interval_ops
DEFAULT FOR TYPE interval USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	1	gbt_intv_consistent (internal, interval, int2, oid, internal),
	FUNCTION	2	gbt_intv_union (internal, internal),
	FUNCTION	3	gbt_intv_compress (internal),
	FUNCTION	4	gbt_intv_decompress (internal),
	FUNCTION	5	gbt_intv_penalty (internal, internal, internal),
	FUNCTION	6	gbt_intv_picksplit (internal, internal),
	FUNCTION	7	gbt_intv_same (gbtreekey32, gbtreekey32, internal),
	FUNCTION	8	gbt_intv_distance (internal, interval, int2, oid, internal),
	FUNCTION	9	gbt_intv_fetch (internal),
	FUNCTION	11	gbt_intv_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey32;


--
--
--
-- cash ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_cash_consistent(internal,money,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_distance(internal,money,int2,oid,internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_cash_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_cash_ops
DEFAULT FOR TYPE money USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	OPERATOR	15	<-> FOR ORDER BY pg_catalog.money_ops ,
	FUNCTION	1	gbt_cash_consistent (internal, money, int2, oid, internal),
	FUNCTION	2	gbt_cash_union (internal, internal),
	FUNCTION	3	gbt_cash_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_cash_penalty (internal, internal, internal),
	FUNCTION	6	gbt_cash_picksplit (internal, internal),
	FUNCTION	7	gbt_cash_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	8	gbt_cash_distance (internal, money, int2, oid, internal),
	FUNCTION	9	gbt_cash_fetch (internal),
	FUNCTION	11	gbt_cash_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- macaddr ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_macad_consistent(internal,macaddr,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macaddr_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_macaddr_ops
DEFAULT FOR TYPE macaddr USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_macad_consistent (internal, macaddr, int2, oid, internal),
	FUNCTION	2	gbt_macad_union (internal, internal),
	FUNCTION	3	gbt_macad_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_macad_penalty (internal, internal, internal),
	FUNCTION	6	gbt_macad_picksplit (internal, internal),
	FUNCTION	7	gbt_macad_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	9	gbt_macad_fetch (internal),
	FUNCTION	11	gbt_macaddr_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- text/bpchar ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_text_consistent(internal,text,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bpchar_consistent(internal,bpchar,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_text_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bpchar_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_text_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_text_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_text_union(internal, internal)
RETURNS gbtreekey_var
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_text_same(gbtreekey_var, gbtreekey_var, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_text_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bpchar_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_text_ops
DEFAULT FOR TYPE text USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_text_consistent (internal, text, int2, oid, internal),
	FUNCTION	2	gbt_text_union (internal, internal),
	FUNCTION	3	gbt_text_compress (internal),
	FUNCTION	4	gbt_var_decompress (internal),
	FUNCTION	5	gbt_text_penalty (internal, internal, internal),
	FUNCTION	6	gbt_text_picksplit (internal, internal),
	FUNCTION	7	gbt_text_same (gbtreekey_var, gbtreekey_var, internal),
	FUNCTION	9	gbt_var_fetch (internal),
	FUNCTION	11	gbt_text_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE			gbtreekey_var;

---- Create the operator class
CREATE OPERATOR CLASS gist_bpchar_ops
DEFAULT FOR TYPE bpchar USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_bpchar_consistent (internal, bpchar , int2, oid, internal),
	FUNCTION	2	gbt_text_union (internal, internal),
	FUNCTION	3	gbt_bpchar_compress (internal),
	FUNCTION	4	gbt_var_decompress (internal),
	FUNCTION	5	gbt_text_penalty (internal, internal, internal),
	FUNCTION	6	gbt_text_picksplit (internal, internal),
	FUNCTION	7	gbt_text_same (gbtreekey_var, gbtreekey_var, internal),
	FUNCTION	9	gbt_var_fetch (internal),
	FUNCTION	11	gbt_bpchar_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE			gbtreekey_var;


--
--
-- bytea ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_bytea_consistent(internal,bytea,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bytea_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bytea_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bytea_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bytea_union(internal, internal)
RETURNS gbtreekey_var
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bytea_same(gbtreekey_var, gbtreekey_var, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bytea_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_bytea_ops
DEFAULT FOR TYPE bytea USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_bytea_consistent (internal, bytea, int2, oid, internal),
	FUNCTION	2	gbt_bytea_union (internal, internal),
	FUNCTION	3	gbt_bytea_compress (internal),
	FUNCTION	4	gbt_var_decompress (internal),
	FUNCTION	5	gbt_bytea_penalty (internal, internal, internal),
	FUNCTION	6	gbt_bytea_picksplit (internal, internal),
	FUNCTION	7	gbt_bytea_same (gbtreekey_var, gbtreekey_var, internal),
	FUNCTION	9	gbt_var_fetch (internal),
	FUNCTION	11	gbt_bytea_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE			gbtreekey_var;


--
--
--
-- numeric ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_numeric_consistent(internal,numeric,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_numeric_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_numeric_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_numeric_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_numeric_union(internal, internal)
RETURNS gbtreekey_var
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_numeric_same(gbtreekey_var, gbtreekey_var, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_numeric_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_numeric_ops
DEFAULT FOR TYPE numeric USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_numeric_consistent (internal, numeric, int2, oid, internal),
	FUNCTION	2	gbt_numeric_union (internal, internal),
	FUNCTION	3	gbt_numeric_compress (internal),
	FUNCTION	4	gbt_var_decompress (internal),
	FUNCTION	5	gbt_numeric_penalty (internal, internal, internal),
	FUNCTION	6	gbt_numeric_picksplit (internal, internal),
	FUNCTION	7	gbt_numeric_same (gbtreekey_var, gbtreekey_var, internal),
	FUNCTION	9	gbt_var_fetch (internal),
	FUNCTION	11	gbt_numeric_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE			gbtreekey_var;


--
--
-- bit ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_bit_consistent(internal,bit,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bit_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bit_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bit_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bit_union(internal, internal)
RETURNS gbtreekey_var
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bit_same(gbtreekey_var, gbtreekey_var, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bit_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_varbit_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_bit_ops
DEFAULT FOR TYPE bit USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_bit_consistent (internal, bit, int2, oid, internal),
	FUNCTION	2	gbt_bit_union (internal, internal),
	FUNCTION	3	gbt_bit_compress (internal),
	FUNCTION	4	gbt_var_decompress (internal),
	FUNCTION	5	gbt_bit_penalty (internal, internal, internal),
	FUNCTION	6	gbt_bit_picksplit (internal, internal),
	FUNCTION	7	gbt_bit_same (gbtreekey_var, gbtreekey_var, internal),
	FUNCTION	9	gbt_var_fetch (internal),
	FUNCTION	11	gbt_bit_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE			gbtreekey_var;

-- Create the operator class
CREATE OPERATOR CLASS gist_vbit_ops
DEFAULT FOR TYPE varbit USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_bit_consistent (internal, bit, int2, oid, internal),
	FUNCTION	2	gbt_bit_union (internal, internal),
	FUNCTION	3	gbt_bit_compress (internal),
	FUNCTION	4	gbt_var_decompress (internal),
	FUNCTION	5	gbt_bit_penalty (internal, internal, internal),
	FUNCTION	6	gbt_bit_picksplit (internal, internal),
	FUNCTION	7	gbt_bit_same (gbtreekey_var, gbtreekey_var, internal),
	FUNCTION	9	gbt_var_fetch (internal),
	FUNCTION	11	gbt_varbit_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE			gbtreekey_var;


--
--
--
-- inet/cidr ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_inet_consistent(internal,inet,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_inet_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_inet_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_inet_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_inet_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_inet_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_inet_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class (intentionally not DEFAULT)
CREATE OPERATOR CLASS gist_inet_ops
FOR TYPE inet USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_inet_consistent (internal, inet, int2, oid, internal),
	FUNCTION	2	gbt_inet_union (internal, internal),
	FUNCTION	3	gbt_inet_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_inet_penalty (internal, internal, internal),
	FUNCTION	6	gbt_inet_picksplit (internal, internal),
	FUNCTION	7	gbt_inet_same (gbtreekey16, gbtreekey16, internal),
	-- no fetch support, the compress function is lossy
	FUNCTION	11	gbt_inet_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;

-- Create the operator class (intentionally not DEFAULT)
CREATE OPERATOR CLASS gist_cidr_ops
FOR TYPE cidr USING gist
AS
	OPERATOR	1	<  (inet, inet) ,
	OPERATOR	2	<= (inet, inet) ,
	OPERATOR	3	=  (inet, inet) ,
	OPERATOR	4	>= (inet, inet) ,
	OPERATOR	5	>  (inet, inet) ,
	OPERATOR	6	<> (inet, inet) ,
	FUNCTION	1	gbt_inet_consistent (internal, inet, int2, oid, internal),
	FUNCTION	2	gbt_inet_union (internal, internal),
	FUNCTION	3	gbt_inet_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_inet_penalty (internal, internal, internal),
	FUNCTION	6	gbt_inet_picksplit (internal, internal),
	FUNCTION	7	gbt_inet_same (gbtreekey16, gbtreekey16, internal),
	-- no fetch support, the compress function is lossy
	FUNCTION	11	gbt_inet_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


--
--
--
-- uuid ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_uuid_consistent(internal,uuid,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_union(internal, internal)
RETURNS gbtreekey32
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_same(gbtreekey32, gbtreekey32, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_uuid_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_uuid_ops
DEFAULT FOR TYPE uuid USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_uuid_consistent (internal, uuid, int2, oid, internal),
	FUNCTION	2	gbt_uuid_union (internal, internal),
	FUNCTION	3	gbt_uuid_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_uuid_penalty (internal, internal, internal),
	FUNCTION	6	gbt_uuid_picksplit (internal, internal),
	FUNCTION	7	gbt_uuid_same (gbtreekey32, gbtreekey32, internal),
	FUNCTION	9	gbt_uuid_fetch (internal),
	FUNCTION	11	gbt_uuid_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey32;


--
--
--
-- macaddr8 ops
--
--
--
-- define the GiST support methods
CREATE FUNCTION gbt_macad8_consistent(internal,macaddr8,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_union(internal, internal)
RETURNS gbtreekey16
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_same(gbtreekey16, gbtreekey16, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_macad8_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_macaddr8_ops
DEFAULT FOR TYPE macaddr8 USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_macad8_consistent (internal, macaddr8, int2, oid, internal),
	FUNCTION	2	gbt_macad8_union (internal, internal),
	FUNCTION	3	gbt_macad8_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_macad8_penalty (internal, internal, internal),
	FUNCTION	6	gbt_macad8_picksplit (internal, internal),
	FUNCTION	7	gbt_macad8_same (gbtreekey16, gbtreekey16, internal),
	FUNCTION	9	gbt_macad8_fetch (internal),
	FUNCTION	11	gbt_macad8_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey16;


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
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_union(internal, internal)
RETURNS gbtreekey8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_same(gbtreekey8, gbtreekey8, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_enum_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the operator class
CREATE OPERATOR CLASS gist_enum_ops
DEFAULT FOR TYPE anyenum USING gist
AS
	OPERATOR	1	<  ,
	OPERATOR	2	<= ,
	OPERATOR	3	=  ,
	OPERATOR	4	>= ,
	OPERATOR	5	>  ,
	OPERATOR	6	<> ,
	FUNCTION	1	gbt_enum_consistent (internal, anyenum, int2, oid, internal),
	FUNCTION	2	gbt_enum_union (internal, internal),
	FUNCTION	3	gbt_enum_compress (internal),
	FUNCTION	4	gbt_decompress (internal),
	FUNCTION	5	gbt_enum_penalty (internal, internal, internal),
	FUNCTION	6	gbt_enum_picksplit (internal, internal),
	FUNCTION	7	gbt_enum_same (gbtreekey8, gbtreekey8, internal),
	FUNCTION	9	gbt_enum_fetch (internal),
	FUNCTION	11	gbt_enum_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey8;


--
--
--
-- bool ops
--
--
--
-- Define the GiST support methods
CREATE FUNCTION gbt_bool_consistent(internal,bool,int2,oid,internal)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_compress(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_fetch(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_penalty(internal,internal,internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_picksplit(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_union(internal, internal)
RETURNS gbtreekey2
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_same(gbtreekey2, gbtreekey2, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION gbt_bool_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

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
	FUNCTION	7	gbt_bool_same (gbtreekey2, gbtreekey2, internal),
	FUNCTION	9	gbt_bool_fetch (internal),
	FUNCTION	11	gbt_bool_sortsupport (internal),
	FUNCTION	12 ("any", "any") gist_translate_cmptype_btree (int),
	STORAGE		gbtreekey2;
