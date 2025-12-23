/* contrib/btree_gist/btree_gist--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.8'" to load this file. \quit

-- Add sortsupport functions

CREATE FUNCTION gbt_bit_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_varbit_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_bool_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_bytea_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_cash_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_date_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_enum_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_float4_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_float8_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_inet_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_int2_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_int4_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_int8_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_intv_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_macaddr_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_macad8_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_numeric_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_oid_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_text_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_bpchar_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_time_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_ts_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION gbt_uuid_sortsupport(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

ALTER OPERATOR FAMILY gist_bit_ops USING gist ADD
	FUNCTION	11  (bit, bit) gbt_bit_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_vbit_ops USING gist ADD
	FUNCTION	11  (varbit, varbit) gbt_varbit_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_bool_ops USING gist ADD
	FUNCTION	11  (bool, bool) gbt_bool_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_bytea_ops USING gist ADD
	FUNCTION	11  (bytea, bytea) gbt_bytea_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_cash_ops USING gist ADD
	FUNCTION	11  (money, money) gbt_cash_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_date_ops USING gist ADD
	FUNCTION	11  (date, date) gbt_date_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_enum_ops USING gist ADD
	FUNCTION	11  (anyenum, anyenum) gbt_enum_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_float4_ops USING gist ADD
	FUNCTION	11  (float4, float4) gbt_float4_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_float8_ops USING gist ADD
	FUNCTION	11  (float8, float8) gbt_float8_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_inet_ops USING gist ADD
	FUNCTION	11  (inet, inet) gbt_inet_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_cidr_ops USING gist ADD
	FUNCTION	11  (cidr, cidr) gbt_inet_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_int2_ops USING gist ADD
	FUNCTION	11  (int2, int2) gbt_int2_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_int4_ops USING gist ADD
	FUNCTION	11  (int4, int4) gbt_int4_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_int8_ops USING gist ADD
	FUNCTION	11  (int8, int8) gbt_int8_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_interval_ops USING gist ADD
	FUNCTION	11  (interval, interval) gbt_intv_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_macaddr_ops USING gist ADD
	FUNCTION	11  (macaddr, macaddr) gbt_macaddr_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_macaddr8_ops USING gist ADD
	FUNCTION	11  (macaddr8, macaddr8) gbt_macad8_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_numeric_ops USING gist ADD
	FUNCTION	11  (numeric, numeric) gbt_numeric_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_oid_ops USING gist ADD
	FUNCTION	11  (oid, oid) gbt_oid_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_text_ops USING gist ADD
	FUNCTION	11  (text, text) gbt_text_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_bpchar_ops USING gist ADD
	FUNCTION	11  (bpchar, bpchar) gbt_bpchar_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_time_ops USING gist ADD
	FUNCTION	11  (time, time) gbt_time_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_timetz_ops USING gist ADD
	FUNCTION	11  (timetz, timetz) gbt_time_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_timestamp_ops USING gist ADD
	FUNCTION	11  (timestamp, timestamp) gbt_ts_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_timestamptz_ops USING gist ADD
	FUNCTION	11  (timestamptz, timestamptz) gbt_ts_sortsupport (internal) ;

ALTER OPERATOR FAMILY gist_uuid_ops USING gist ADD
	FUNCTION	11  (uuid, uuid) gbt_uuid_sortsupport (internal) ;

-- Add translate_cmptype functions

CREATE FUNCTION gist_translate_cmptype_btree(int)
RETURNS smallint
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

ALTER OPERATOR FAMILY gist_oid_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_int2_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_int4_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_int8_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_float4_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_float8_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_timestamp_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_timestamptz_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_time_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_date_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_interval_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_cash_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_macaddr_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_text_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_bpchar_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_bytea_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_numeric_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_bit_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_vbit_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_inet_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_cidr_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_timetz_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_uuid_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_macaddr8_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_enum_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;

ALTER OPERATOR FAMILY gist_bool_ops USING gist ADD
	FUNCTION 12 ("any", "any") gist_translate_cmptype_btree (int) ;
