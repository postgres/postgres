SET search_path = public;

BEGIN;

DROP OPERATOR CLASS gist_trgm_ops;

DROP FUNCTION gtrgm_same(gtrgm, gtrgm, internal);

DROP FUNCTION gtrgm_union(bytea, internal);

DROP FUNCTION gtrgm_picksplit(internal, internal);

DROP FUNCTION gtrgm_penalty(internal,internal,internal);

DROP FUNCTION gtrgm_decompress(internal);

DROP FUNCTION gtrgm_compress(internal);
 
DROP FUNCTION gtrgm_consistent(gtrgm,internal,int4);

DROP TYPE gtrgm;

DROP FUNCTION gtrgm_out(gtrgm);

DROP FUNCTION gtrgm_in(cstring);

DROP OPERATOR % (text, text);

DROP FUNCTION similarity_op(text,text);

DROP FUNCTION similarity(text,text);

DROP FUNCTION show_trgm(text);

DROP FUNCTION show_limit();

DROP FUNCTION set_limit(float4);

COMMIT;
