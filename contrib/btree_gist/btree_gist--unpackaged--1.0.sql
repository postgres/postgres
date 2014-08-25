/* contrib/btree_gist/btree_gist--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION btree_gist FROM unpackaged" to load this file. \quit

ALTER EXTENSION btree_gist ADD type gbtreekey4;
ALTER EXTENSION btree_gist ADD function gbtreekey4_in(cstring);
ALTER EXTENSION btree_gist ADD function gbtreekey4_out(gbtreekey4);
ALTER EXTENSION btree_gist ADD type gbtreekey8;
ALTER EXTENSION btree_gist ADD function gbtreekey8_in(cstring);
ALTER EXTENSION btree_gist ADD function gbtreekey8_out(gbtreekey8);
ALTER EXTENSION btree_gist ADD type gbtreekey16;
ALTER EXTENSION btree_gist ADD function gbtreekey16_in(cstring);
ALTER EXTENSION btree_gist ADD function gbtreekey16_out(gbtreekey16);
ALTER EXTENSION btree_gist ADD type gbtreekey32;
ALTER EXTENSION btree_gist ADD function gbtreekey32_in(cstring);
ALTER EXTENSION btree_gist ADD function gbtreekey32_out(gbtreekey32);
ALTER EXTENSION btree_gist ADD type gbtreekey_var;
ALTER EXTENSION btree_gist ADD function gbtreekey_var_in(cstring);
ALTER EXTENSION btree_gist ADD function gbtreekey_var_out(gbtreekey_var);
ALTER EXTENSION btree_gist ADD function gbt_oid_consistent(internal,oid,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_oid_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_decompress(internal);
ALTER EXTENSION btree_gist ADD function gbt_var_decompress(internal);
ALTER EXTENSION btree_gist ADD function gbt_oid_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_oid_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_oid_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_oid_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_oid_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_oid_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_int2_consistent(internal,smallint,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_int2_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_int2_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_int2_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_int2_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_int2_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_int2_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_int2_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_int4_consistent(internal,integer,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_int4_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_int4_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_int4_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_int4_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_int4_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_int4_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_int4_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_int8_consistent(internal,bigint,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_int8_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_int8_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_int8_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_int8_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_int8_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_int8_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_int8_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_float4_consistent(internal,real,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_float4_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_float4_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_float4_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_float4_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_float4_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_float4_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_float4_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_float8_consistent(internal,double precision,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_float8_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_float8_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_float8_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_float8_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_float8_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_float8_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_float8_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_ts_consistent(internal,timestamp without time zone,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_tstz_consistent(internal,timestamp with time zone,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_ts_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_tstz_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_ts_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_ts_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_ts_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_ts_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_timestamp_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_timestamp_ops using gist;
ALTER EXTENSION btree_gist ADD operator family gist_timestamptz_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_timestamptz_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_time_consistent(internal,time without time zone,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_timetz_consistent(internal,time with time zone,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_time_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_timetz_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_time_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_time_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_time_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_time_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_time_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_time_ops using gist;
ALTER EXTENSION btree_gist ADD operator family gist_timetz_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_timetz_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_date_consistent(internal,date,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_date_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_date_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_date_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_date_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_date_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_date_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_date_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_intv_consistent(internal,interval,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_intv_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_intv_decompress(internal);
ALTER EXTENSION btree_gist ADD function gbt_intv_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_intv_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_intv_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_intv_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_interval_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_interval_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_cash_consistent(internal,money,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_cash_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_cash_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_cash_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_cash_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_cash_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_cash_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_cash_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_macad_consistent(internal,macaddr,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_macad_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_macad_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_macad_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_macad_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_macad_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_macaddr_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_macaddr_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_text_consistent(internal,text,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_bpchar_consistent(internal,character,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_text_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_bpchar_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_text_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_text_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_text_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_text_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_text_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_text_ops using gist;
ALTER EXTENSION btree_gist ADD operator family gist_bpchar_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_bpchar_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_bytea_consistent(internal,bytea,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_bytea_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_bytea_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_bytea_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_bytea_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_bytea_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_bytea_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_bytea_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_numeric_consistent(internal,numeric,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_numeric_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_numeric_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_numeric_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_numeric_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_numeric_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_numeric_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_numeric_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_bit_consistent(internal,bit,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_bit_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_bit_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_bit_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_bit_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_bit_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_bit_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_bit_ops using gist;
ALTER EXTENSION btree_gist ADD operator family gist_vbit_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_vbit_ops using gist;
ALTER EXTENSION btree_gist ADD function gbt_inet_consistent(internal,inet,smallint,oid,internal);
ALTER EXTENSION btree_gist ADD function gbt_inet_compress(internal);
ALTER EXTENSION btree_gist ADD function gbt_inet_penalty(internal,internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_inet_picksplit(internal,internal);
ALTER EXTENSION btree_gist ADD function gbt_inet_union(bytea,internal);
ALTER EXTENSION btree_gist ADD function gbt_inet_same(internal,internal,internal);
ALTER EXTENSION btree_gist ADD operator family gist_inet_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_inet_ops using gist;
ALTER EXTENSION btree_gist ADD operator family gist_cidr_ops using gist;
ALTER EXTENSION btree_gist ADD operator class gist_cidr_ops using gist;


-- Add functions and operators that are new in 9.1

--distance operators

CREATE FUNCTION cash_dist(money, money)
RETURNS money
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = money,
	RIGHTARG = money,
	PROCEDURE = cash_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION date_dist(date, date)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = date,
	RIGHTARG = date,
	PROCEDURE = date_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION float4_dist(float4, float4)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = float4,
	RIGHTARG = float4,
	PROCEDURE = float4_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION float8_dist(float8, float8)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = float8,
	RIGHTARG = float8,
	PROCEDURE = float8_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION int2_dist(int2, int2)
RETURNS int2
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = int2,
	RIGHTARG = int2,
	PROCEDURE = int2_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION int4_dist(int4, int4)
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = int4,
	RIGHTARG = int4,
	PROCEDURE = int4_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION int8_dist(int8, int8)
RETURNS int8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = int8,
	RIGHTARG = int8,
	PROCEDURE = int8_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION interval_dist(interval, interval)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = interval,
	RIGHTARG = interval,
	PROCEDURE = interval_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION oid_dist(oid, oid)
RETURNS oid
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = oid,
	RIGHTARG = oid,
	PROCEDURE = oid_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION time_dist(time, time)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = time,
	RIGHTARG = time,
	PROCEDURE = time_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION ts_dist(timestamp, timestamp)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = timestamp,
	RIGHTARG = timestamp,
	PROCEDURE = ts_dist,
	COMMUTATOR = '<->'
);

CREATE FUNCTION tstz_dist(timestamptz, timestamptz)
RETURNS interval
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR <-> (
	LEFTARG = timestamptz,
	RIGHTARG = timestamptz,
	PROCEDURE = tstz_dist,
	COMMUTATOR = '<->'
);

-- Support functions for distance operators

CREATE FUNCTION gbt_oid_distance(internal,oid,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_int2_distance(internal,int2,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_int4_distance(internal,int4,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_int8_distance(internal,int8,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_float4_distance(internal,float4,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_float8_distance(internal,float8,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_ts_distance(internal,timestamp,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_tstz_distance(internal,timestamptz,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_time_distance(internal,time,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_date_distance(internal,date,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_intv_distance(internal,interval,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION gbt_cash_distance(internal,money,int2,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;


-- Add new-in-9.1 stuff to the operator classes.

ALTER OPERATOR FAMILY gist_oid_ops USING gist ADD
	OPERATOR	6	<> (oid, oid) ,
	OPERATOR	15	<-> (oid, oid) FOR ORDER BY pg_catalog.oid_ops ,
	FUNCTION	8 (oid, oid) gbt_oid_distance (internal, oid, int2, oid) ;

ALTER OPERATOR FAMILY gist_int2_ops USING gist ADD
	OPERATOR	6	<> (int2, int2) ,
	OPERATOR	15	<-> (int2, int2) FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	8 (int2, int2) gbt_int2_distance (internal, int2, int2, oid) ;

ALTER OPERATOR FAMILY gist_int4_ops USING gist ADD
	OPERATOR	6	<> (int4, int4) ,
	OPERATOR	15	<-> (int4, int4) FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	8 (int4, int4) gbt_int4_distance (internal, int4, int2, oid) ;

ALTER OPERATOR FAMILY gist_int8_ops USING gist ADD
	OPERATOR	6	<> (int8, int8) ,
	OPERATOR	15	<-> (int8, int8) FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	8 (int8, int8) gbt_int8_distance (internal, int8, int2, oid) ;

ALTER OPERATOR FAMILY gist_float4_ops USING gist ADD
	OPERATOR	6	<> (float4, float4) ,
	OPERATOR	15	<-> (float4, float4) FOR ORDER BY pg_catalog.float_ops ,
	FUNCTION	8 (float4, float4) gbt_float4_distance (internal, float4, int2, oid) ;

ALTER OPERATOR FAMILY gist_float8_ops USING gist ADD
	OPERATOR	6	<> (float8, float8) ,
	OPERATOR	15	<-> (float8, float8) FOR ORDER BY pg_catalog.float_ops ,
	FUNCTION	8 (float8, float8) gbt_float8_distance (internal, float8, int2, oid) ;

ALTER OPERATOR FAMILY gist_timestamp_ops USING gist ADD
	OPERATOR	6	<> (timestamp, timestamp) ,
	OPERATOR	15	<-> (timestamp, timestamp) FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	8 (timestamp, timestamp) gbt_ts_distance (internal, timestamp, int2, oid) ;

ALTER OPERATOR FAMILY gist_timestamptz_ops USING gist ADD
	OPERATOR	6	<> (timestamptz, timestamptz) ,
	OPERATOR	15	<-> (timestamptz, timestamptz) FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	8 (timestamptz, timestamptz) gbt_tstz_distance (internal, timestamptz, int2, oid) ;

ALTER OPERATOR FAMILY gist_time_ops USING gist ADD
	OPERATOR	6	<> (time, time) ,
	OPERATOR	15	<-> (time, time) FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	8 (time, time) gbt_time_distance (internal, time, int2, oid) ;

ALTER OPERATOR FAMILY gist_timetz_ops USING gist ADD
	OPERATOR	6	<> (timetz, timetz) ;

ALTER OPERATOR FAMILY gist_date_ops USING gist ADD
	OPERATOR	6	<> (date, date) ,
	OPERATOR	15	<-> (date, date) FOR ORDER BY pg_catalog.integer_ops ,
	FUNCTION	8 (date, date) gbt_date_distance (internal, date, int2, oid) ;

ALTER OPERATOR FAMILY gist_interval_ops USING gist ADD
	OPERATOR	6	<> (interval, interval) ,
	OPERATOR	15	<-> (interval, interval) FOR ORDER BY pg_catalog.interval_ops ,
	FUNCTION	8 (interval, interval) gbt_intv_distance (internal, interval, int2, oid) ;

ALTER OPERATOR FAMILY gist_cash_ops USING gist ADD
	OPERATOR	6	<> (money, money) ,
	OPERATOR	15	<-> (money, money) FOR ORDER BY pg_catalog.money_ops ,
	FUNCTION	8 (money, money) gbt_cash_distance (internal, money, int2, oid) ;

ALTER OPERATOR FAMILY gist_macaddr_ops USING gist ADD
	OPERATOR	6	<> (macaddr, macaddr) ;

ALTER OPERATOR FAMILY gist_text_ops USING gist ADD
	OPERATOR	6	<> (text, text) ;

ALTER OPERATOR FAMILY gist_bpchar_ops USING gist ADD
	OPERATOR	6	<> (bpchar, bpchar) ;

ALTER OPERATOR FAMILY gist_bytea_ops USING gist ADD
	OPERATOR	6	<> (bytea, bytea) ;

ALTER OPERATOR FAMILY gist_numeric_ops USING gist ADD
	OPERATOR	6	<> (numeric, numeric) ;

ALTER OPERATOR FAMILY gist_bit_ops USING gist ADD
	OPERATOR	6	<> (bit, bit) ;

ALTER OPERATOR FAMILY gist_vbit_ops USING gist ADD
	OPERATOR	6	<> (varbit, varbit) ;

ALTER OPERATOR FAMILY gist_inet_ops USING gist ADD
	OPERATOR	6	<>  (inet, inet) ;

ALTER OPERATOR FAMILY gist_cidr_ops USING gist ADD
	OPERATOR	6	<> (inet, inet) ;
