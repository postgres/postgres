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
