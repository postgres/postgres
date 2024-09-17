/* contrib/btree_gist/btree_gist--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.8'" to load this file. \quit

CREATE FUNCTION gist_stratnum_btree(smallint)
RETURNS smallint
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

ALTER OPERATOR FAMILY gist_oid_ops USING gist ADD
	FUNCTION 12 (oid, oid) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_int2_ops USING gist ADD
	FUNCTION 12 (int2, int2) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_int4_ops USING gist ADD
	FUNCTION 12 (int4, int4) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_int8_ops USING gist ADD
	FUNCTION 12 (int8, int8) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_float4_ops USING gist ADD
	FUNCTION 12 (float4, float4) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_float8_ops USING gist ADD
	FUNCTION 12 (float8, float8) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_timestamp_ops USING gist ADD
	FUNCTION 12 (timestamp, timestamp) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_timestamptz_ops USING gist ADD
	FUNCTION 12 (timestamptz, timestamptz) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_time_ops USING gist ADD
	FUNCTION 12 (time, time) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_date_ops USING gist ADD
	FUNCTION 12 (date, date) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_interval_ops USING gist ADD
	FUNCTION 12 (interval, interval) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_cash_ops USING gist ADD
	FUNCTION 12 (money, money) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_macaddr_ops USING gist ADD
	FUNCTION 12 (macaddr, macaddr) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_text_ops USING gist ADD
	FUNCTION 12 (text, text) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_bpchar_ops USING gist ADD
	FUNCTION 12 (bpchar, bpchar) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_bytea_ops USING gist ADD
	FUNCTION 12 (bytea, bytea) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_numeric_ops USING gist ADD
	FUNCTION 12 (numeric, numeric) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_bit_ops USING gist ADD
	FUNCTION 12 (bit, bit) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_vbit_ops USING gist ADD
	FUNCTION 12 (varbit, varbit) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_inet_ops USING gist ADD
	FUNCTION 12 (inet, inet) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_cidr_ops USING gist ADD
	FUNCTION 12 (cidr, cidr) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_timetz_ops USING gist ADD
	FUNCTION 12 (timetz, timetz) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_uuid_ops USING gist ADD
	FUNCTION 12 (uuid, uuid) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_macaddr8_ops USING gist ADD
	FUNCTION 12 (macaddr8, macaddr8) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_enum_ops USING gist ADD
	FUNCTION 12 (anyenum, anyenum) gist_stratnum_btree (int2) ;

ALTER OPERATOR FAMILY gist_bool_ops USING gist ADD
	FUNCTION 12 (bool, bool) gist_stratnum_btree (int2) ;
