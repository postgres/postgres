/* contrib/btree_gist/btree_gist--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.2'" to load this file. \quit

-- Update procedure signatures the hard way.
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
-- return NULL and no updates will happen.

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.regtype[]), -- establish column types
('gbt_oid_distance(internal,oid,int2,oid)', '{internal,oid,int2,oid,internal}'),
('gbt_oid_union(bytea,internal)', '{internal,internal}'),
('gbt_oid_same(internal,internal,internal)', '{gbtreekey8,gbtreekey8,internal}'),
('gbt_int2_distance(internal,int2,int2,oid)', '{internal,int2,int2,oid,internal}'),
('gbt_int2_union(bytea,internal)', '{internal,internal}'),
('gbt_int2_same(internal,internal,internal)', '{gbtreekey4,gbtreekey4,internal}'),
('gbt_int4_distance(internal,int4,int2,oid)', '{internal,int4,int2,oid,internal}'),
('gbt_int4_union(bytea,internal)', '{internal,internal}'),
('gbt_int4_same(internal,internal,internal)', '{gbtreekey8,gbtreekey8,internal}'),
('gbt_int8_distance(internal,int8,int2,oid)', '{internal,int8,int2,oid,internal}'),
('gbt_int8_union(bytea,internal)', '{internal,internal}'),
('gbt_int8_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}'),
('gbt_float4_distance(internal,float4,int2,oid)', '{internal,float4,int2,oid,internal}'),
('gbt_float4_union(bytea,internal)', '{internal,internal}'),
('gbt_float4_same(internal,internal,internal)', '{gbtreekey8,gbtreekey8,internal}'),
('gbt_float8_distance(internal,float8,int2,oid)', '{internal,float8,int2,oid,internal}'),
('gbt_float8_union(bytea,internal)', '{internal,internal}'),
('gbt_float8_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}'),
('gbt_ts_distance(internal,timestamp,int2,oid)', '{internal,timestamp,int2,oid,internal}'),
('gbt_tstz_distance(internal,timestamptz,int2,oid)', '{internal,timestamptz,int2,oid,internal}'),
('gbt_ts_union(bytea,internal)', '{internal,internal}'),
('gbt_ts_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}'),
('gbt_time_distance(internal,time,int2,oid)', '{internal,time,int2,oid,internal}'),
('gbt_time_union(bytea,internal)', '{internal,internal}'),
('gbt_time_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}'),
('gbt_date_distance(internal,date,int2,oid)', '{internal,date,int2,oid,internal}'),
('gbt_date_union(bytea,internal)', '{internal,internal}'),
('gbt_date_same(internal,internal,internal)', '{gbtreekey8,gbtreekey8,internal}'),
('gbt_intv_distance(internal,interval,int2,oid)', '{internal,interval,int2,oid,internal}'),
('gbt_intv_union(bytea,internal)', '{internal,internal}'),
('gbt_intv_same(internal,internal,internal)', '{gbtreekey32,gbtreekey32,internal}'),
('gbt_cash_distance(internal,money,int2,oid)', '{internal,money,int2,oid,internal}'),
('gbt_cash_union(bytea,internal)', '{internal,internal}'),
('gbt_cash_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}'),
('gbt_macad_union(bytea,internal)', '{internal,internal}'),
('gbt_macad_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}'),
('gbt_text_union(bytea,internal)', '{internal,internal}'),
('gbt_text_same(internal,internal,internal)', '{gbtreekey_var,gbtreekey_var,internal}'),
('gbt_bytea_union(bytea,internal)', '{internal,internal}'),
('gbt_bytea_same(internal,internal,internal)', '{gbtreekey_var,gbtreekey_var,internal}'),
('gbt_numeric_union(bytea,internal)', '{internal,internal}'),
('gbt_numeric_same(internal,internal,internal)', '{gbtreekey_var,gbtreekey_var,internal}'),
('gbt_bit_union(bytea,internal)', '{internal,internal}'),
('gbt_bit_same(internal,internal,internal)', '{gbtreekey_var,gbtreekey_var,internal}'),
('gbt_inet_union(bytea,internal)', '{internal,internal}'),
('gbt_inet_same(internal,internal,internal)', '{gbtreekey16,gbtreekey16,internal}')
) AS update_data (oldproc, newtypes)
WHERE oid = pg_catalog.to_regprocedure(oldproc);
