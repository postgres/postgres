SET search_path = public;

DROP OPERATOR CLASS gist_cidr_ops USING gist;

DROP OPERATOR CLASS gist_inet_ops USING gist;

DROP FUNCTION gbt_inet_same(internal, internal, internal);

DROP FUNCTION gbt_inet_union(bytea, internal);

DROP FUNCTION gbt_inet_picksplit(internal, internal);

DROP FUNCTION gbt_inet_penalty(internal,internal,internal);

DROP FUNCTION gbt_inet_compress(internal);

DROP FUNCTION gbt_inet_consistent(internal,inet,int2);

DROP OPERATOR CLASS gist_vbit_ops USING gist;

DROP OPERATOR CLASS gist_bit_ops USING gist;

DROP FUNCTION gbt_bit_same(internal, internal, internal);

DROP FUNCTION gbt_bit_union(bytea, internal);

DROP FUNCTION gbt_bit_picksplit(internal, internal);

DROP FUNCTION gbt_bit_penalty(internal,internal,internal);

DROP FUNCTION gbt_bit_compress(internal);

DROP FUNCTION gbt_bit_consistent(internal,bit,int2);

DROP OPERATOR CLASS gist_numeric_ops USING gist;

DROP FUNCTION gbt_numeric_same(internal, internal, internal);

DROP FUNCTION gbt_numeric_union(bytea, internal);

DROP FUNCTION gbt_numeric_picksplit(internal, internal);

DROP FUNCTION gbt_numeric_penalty(internal,internal,internal);

DROP FUNCTION gbt_numeric_compress(internal);

DROP FUNCTION gbt_numeric_consistent(internal,numeric,int2);

DROP OPERATOR CLASS gist_bytea_ops USING gist;

DROP FUNCTION gbt_bytea_same(internal, internal, internal);

DROP FUNCTION gbt_bytea_union(bytea, internal);

DROP FUNCTION gbt_bytea_picksplit(internal, internal);

DROP FUNCTION gbt_bytea_penalty(internal,internal,internal);

DROP FUNCTION gbt_bytea_compress(internal);

DROP FUNCTION gbt_bytea_consistent(internal,bytea,int2);

DROP OPERATOR CLASS gist_bpchar_ops USING gist;

DROP OPERATOR CLASS gist_text_ops USING gist;

DROP FUNCTION gbt_text_same(internal, internal, internal);

DROP FUNCTION gbt_text_union(bytea, internal);

DROP FUNCTION gbt_text_picksplit(internal, internal);

DROP FUNCTION gbt_text_penalty(internal,internal,internal);

DROP FUNCTION gbt_bpchar_compress(internal);

DROP FUNCTION gbt_text_compress(internal);

DROP FUNCTION gbt_bpchar_consistent(internal,bpchar,int2);

DROP FUNCTION gbt_text_consistent(internal,text,int2);

DROP OPERATOR CLASS gist_macaddr_ops USING gist;

DROP FUNCTION gbt_macad_same(internal, internal, internal);

DROP FUNCTION gbt_macad_union(bytea, internal);

DROP FUNCTION gbt_macad_picksplit(internal, internal);

DROP FUNCTION gbt_macad_penalty(internal,internal,internal);

DROP FUNCTION gbt_macad_compress(internal);

DROP FUNCTION gbt_macad_consistent(internal,macaddr,int2);

DROP OPERATOR CLASS gist_cash_ops USING gist;

DROP FUNCTION gbt_cash_same(internal, internal, internal);

DROP FUNCTION gbt_cash_union(bytea, internal);

DROP FUNCTION gbt_cash_picksplit(internal, internal);

DROP FUNCTION gbt_cash_penalty(internal,internal,internal);

DROP FUNCTION gbt_cash_compress(internal);

DROP FUNCTION gbt_cash_consistent(internal,money,int2);

DROP OPERATOR CLASS gist_interval_ops USING gist;

DROP FUNCTION gbt_intv_same(internal, internal, internal);

DROP FUNCTION gbt_intv_union(bytea, internal);
      
DROP FUNCTION gbt_intv_picksplit(internal, internal);
   
DROP FUNCTION gbt_intv_penalty(internal,internal,internal);

DROP FUNCTION gbt_intv_decompress(internal);

DROP FUNCTION gbt_intv_compress(internal);

DROP FUNCTION gbt_intv_consistent(internal,interval,int2);

DROP OPERATOR CLASS gist_date_ops USING gist;

DROP FUNCTION gbt_date_same(internal, internal, internal);

DROP FUNCTION gbt_date_union(bytea, internal);
      
DROP FUNCTION gbt_date_picksplit(internal, internal);
   
DROP FUNCTION gbt_date_penalty(internal,internal,internal);

DROP FUNCTION gbt_date_compress(internal);

DROP FUNCTION gbt_date_consistent(internal,date,int2);

DROP OPERATOR CLASS gist_timetz_ops USING gist;

DROP OPERATOR CLASS gist_time_ops USING gist;

DROP FUNCTION gbt_time_same(internal, internal, internal);

DROP FUNCTION gbt_time_union(bytea, internal);
      
DROP FUNCTION gbt_time_picksplit(internal, internal);
   
DROP FUNCTION gbt_time_penalty(internal,internal,internal);

DROP FUNCTION gbt_timetz_compress(internal);

DROP FUNCTION gbt_time_compress(internal);

DROP FUNCTION gbt_timetz_consistent(internal,timetz,int2);

DROP FUNCTION gbt_time_consistent(internal,time,int2);

DROP OPERATOR CLASS gist_timestamptz_ops USING gist;

DROP OPERATOR CLASS gist_timestamp_ops USING gist;

DROP FUNCTION gbt_ts_same(internal, internal, internal);

DROP FUNCTION gbt_ts_union(bytea, internal);
      
DROP FUNCTION gbt_ts_picksplit(internal, internal);
   
DROP FUNCTION gbt_ts_penalty(internal,internal,internal);

DROP FUNCTION gbt_tstz_compress(internal);

DROP FUNCTION gbt_ts_compress(internal);
      
DROP FUNCTION gbt_tstz_consistent(internal,timestamptz,int2);

DROP FUNCTION gbt_ts_consistent(internal,timestamp,int2);

DROP OPERATOR CLASS gist_float8_ops USING gist;

DROP FUNCTION gbt_float8_same(internal, internal, internal);

DROP FUNCTION gbt_float8_union(bytea, internal);

DROP FUNCTION gbt_float8_picksplit(internal, internal);

DROP FUNCTION gbt_float8_penalty(internal,internal,internal);

DROP FUNCTION gbt_float8_compress(internal);

DROP FUNCTION gbt_float8_consistent(internal,float8,int2);

DROP OPERATOR CLASS gist_float4_ops USING gist;

DROP FUNCTION gbt_float4_same(internal, internal, internal);

DROP FUNCTION gbt_float4_union(bytea, internal);

DROP FUNCTION gbt_float4_picksplit(internal, internal);

DROP FUNCTION gbt_float4_penalty(internal,internal,internal);

DROP FUNCTION gbt_float4_compress(internal);

DROP FUNCTION gbt_float4_consistent(internal,float4,int2);

DROP OPERATOR CLASS gist_int8_ops USING gist;

DROP FUNCTION gbt_int8_same(internal, internal, internal);

DROP FUNCTION gbt_int8_union(bytea, internal);

DROP FUNCTION gbt_int8_picksplit(internal, internal);

DROP FUNCTION gbt_int8_penalty(internal,internal,internal);

DROP FUNCTION gbt_int8_compress(internal);

DROP FUNCTION gbt_int8_consistent(internal,int8,int2);

DROP OPERATOR CLASS gist_int4_ops USING gist;

DROP FUNCTION gbt_int4_same(internal, internal, internal);

DROP FUNCTION gbt_int4_union(bytea, internal);

DROP FUNCTION gbt_int4_picksplit(internal, internal);

DROP FUNCTION gbt_int4_penalty(internal,internal,internal);

DROP FUNCTION gbt_int4_compress(internal);

DROP FUNCTION gbt_int4_consistent(internal,int4,int2);

DROP OPERATOR CLASS gist_int2_ops USING gist;

DROP FUNCTION gbt_int2_same(internal, internal, internal);

DROP FUNCTION gbt_int2_union(bytea, internal);

DROP FUNCTION gbt_int2_picksplit(internal, internal);

DROP FUNCTION gbt_int2_penalty(internal,internal,internal);

DROP FUNCTION gbt_int2_compress(internal);

DROP FUNCTION gbt_int2_consistent(internal,int2,int2);

DROP OPERATOR CLASS gist_oid_ops USING gist;

DROP FUNCTION gbt_oid_same(internal, internal, internal);

DROP FUNCTION gbt_oid_union(bytea, internal);

DROP FUNCTION gbt_oid_picksplit(internal, internal);

DROP FUNCTION gbt_oid_penalty(internal,internal,internal);

DROP FUNCTION gbt_var_decompress(internal);

DROP FUNCTION gbt_decompress(internal);

DROP FUNCTION gbt_oid_compress(internal);

DROP FUNCTION gbt_oid_consistent(internal,oid,int2);

DROP TYPE gbtreekey_var CASCADE;

DROP TYPE gbtreekey32 CASCADE;

DROP TYPE gbtreekey16 CASCADE;

DROP TYPE gbtreekey8 CASCADE;

DROP TYPE gbtreekey4 CASCADE;
