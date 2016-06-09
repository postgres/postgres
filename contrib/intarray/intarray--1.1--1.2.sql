/* contrib/intarray/intarray--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.2'" to load this file. \quit

-- Update procedure signatures the hard way.
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
-- return NULL and no updates will happen.

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.regtype[]), -- establish column types
('g_int_consistent(internal,_int4,int4,oid,internal)', '{internal,_int4,int2,oid,internal}'),
('g_intbig_consistent(internal,internal,int4,oid,internal)', '{internal,_int4,int2,oid,internal}'),
('g_intbig_same(internal,internal,internal)', '{intbig_gkey,intbig_gkey,internal}'),
('ginint4_queryextract(internal,internal,int2,internal,internal,internal,internal)', '{_int4,internal,int2,internal,internal,internal,internal}'),
('ginint4_consistent(internal,int2,internal,int4,internal,internal,internal,internal)', '{internal,int2,_int4,int4,internal,internal,internal,internal}')
) AS update_data (oldproc, newtypes)
WHERE oid = pg_catalog.to_regprocedure(oldproc);

UPDATE pg_catalog.pg_proc SET
  prorettype = 'intbig_gkey'::pg_catalog.regtype
WHERE oid = pg_catalog.to_regprocedure('g_intbig_union(internal,internal)');
