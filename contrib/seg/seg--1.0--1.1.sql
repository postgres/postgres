/* contrib/seg/seg--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION seg UPDATE TO '1.1'" to load this file. \quit

-- Update procedure signatures the hard way.
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
-- return NULL and no updates will happen.

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.regtype[]), -- establish column types
('gseg_consistent(internal,seg,int4,oid,internal)', '{internal,seg,int2,oid,internal}')
) AS update_data (oldproc, newtypes)
WHERE oid = pg_catalog.to_regprocedure(oldproc);

ALTER FUNCTION seg_in(cstring) PARALLEL SAFE;
ALTER FUNCTION seg_out(seg) PARALLEL SAFE;
ALTER FUNCTION seg_over_left(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_over_right(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_left(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_right(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_lt(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_le(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_gt(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_ge(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_contains(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_contained(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_overlap(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_same(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_different(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_cmp(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_union(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_inter(seg, seg) PARALLEL SAFE;
ALTER FUNCTION seg_size(seg) PARALLEL SAFE;
ALTER FUNCTION seg_center(seg) PARALLEL SAFE;
ALTER FUNCTION seg_upper(seg) PARALLEL SAFE;
ALTER FUNCTION seg_lower(seg) PARALLEL SAFE;
ALTER FUNCTION gseg_consistent(internal, seg, smallint, oid, internal) PARALLEL SAFE;
ALTER FUNCTION gseg_compress(internal) PARALLEL SAFE;
ALTER FUNCTION gseg_decompress(internal) PARALLEL SAFE;
ALTER FUNCTION gseg_penalty(internal, internal, internal) PARALLEL SAFE;
ALTER FUNCTION gseg_picksplit(internal, internal) PARALLEL SAFE;
ALTER FUNCTION gseg_union(internal, internal) PARALLEL SAFE;
ALTER FUNCTION gseg_same(seg, seg, internal) PARALLEL SAFE;
