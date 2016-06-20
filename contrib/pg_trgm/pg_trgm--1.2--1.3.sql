/* contrib/pg_trgm/pg_trgm--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_trgm UPDATE TO '1.3'" to load this file. \quit

-- Update procedure signatures the hard way.
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
-- return NULL and no updates will happen.

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.regtype[]), -- establish column types
('gtrgm_consistent(internal,text,int4,oid,internal)', '{internal,text,int2,oid,internal}'),
('gtrgm_distance(internal,text,int4,oid)', '{internal,text,int2,oid,internal}'),
('gtrgm_union(bytea,internal)', '{internal,internal}')
) AS update_data (oldproc, newtypes)
WHERE oid = pg_catalog.to_regprocedure(oldproc);

UPDATE pg_catalog.pg_proc SET
  prorettype = 'gtrgm'::pg_catalog.regtype
WHERE oid = pg_catalog.to_regprocedure('gtrgm_union(internal,internal)');

ALTER FUNCTION set_limit(float4) PARALLEL UNSAFE;
ALTER FUNCTION show_limit() PARALLEL SAFE;
ALTER FUNCTION show_trgm(text) PARALLEL SAFE;
ALTER FUNCTION similarity(text, text) PARALLEL SAFE;
ALTER FUNCTION similarity_op(text, text) PARALLEL SAFE;
ALTER FUNCTION word_similarity(text, text) PARALLEL SAFE;
ALTER FUNCTION word_similarity_op(text, text) PARALLEL SAFE;
ALTER FUNCTION word_similarity_commutator_op(text, text) PARALLEL SAFE;
ALTER FUNCTION similarity_dist(text, text) PARALLEL SAFE;
ALTER FUNCTION word_similarity_dist_op(text, text) PARALLEL SAFE;
ALTER FUNCTION word_similarity_dist_commutator_op(text, text) PARALLEL SAFE;
ALTER FUNCTION gtrgm_in(cstring) PARALLEL SAFE;
ALTER FUNCTION gtrgm_out(gtrgm) PARALLEL SAFE;
ALTER FUNCTION gtrgm_consistent(internal, text, smallint, oid, internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_distance(internal, text, smallint, oid, internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_compress(internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_decompress(internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_penalty(internal, internal, internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_picksplit(internal, internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_union(internal, internal) PARALLEL SAFE;
ALTER FUNCTION gtrgm_same(gtrgm, gtrgm, internal) PARALLEL SAFE;
ALTER FUNCTION gin_extract_value_trgm(text, internal) PARALLEL SAFE;
ALTER FUNCTION gin_extract_query_trgm(text, internal, int2, internal, internal, internal, internal) PARALLEL SAFE;
ALTER FUNCTION gin_trgm_consistent(internal, int2, text, int4, internal, internal, internal, internal) PARALLEL SAFE;
ALTER FUNCTION gin_trgm_triconsistent(internal, int2, text, int4, internal, internal, internal) PARALLEL SAFE;
