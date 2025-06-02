/* contrib/btree_gist/btree_gist--1.7--1.8.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.8'" to load this file. \quit

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
