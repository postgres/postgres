/* contrib/ltree/ltree--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION ltree UPDATE TO '1.1'" to load this file. \quit

-- Update procedure signatures the hard way.
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
-- return NULL and no updates will happen.

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.regtype[]), -- establish column types
('ltree_consistent(internal,internal,int2,oid,internal)', '{internal,ltree,int2,oid,internal}'),
('ltree_same(internal,internal,internal)', '{ltree_gist,ltree_gist,internal}'),
('_ltree_consistent(internal,internal,int2,oid,internal)', '{internal,_ltree,int2,oid,internal}'),
('_ltree_same(internal,internal,internal)', '{ltree_gist,ltree_gist,internal}')
) AS update_data (oldproc, newtypes)
WHERE oid = pg_catalog.to_regprocedure(oldproc);

UPDATE pg_catalog.pg_proc SET
  prorettype = 'ltree_gist'::pg_catalog.regtype
WHERE oid = pg_catalog.to_regprocedure('ltree_union(internal,internal)');

UPDATE pg_catalog.pg_proc SET
  prorettype = 'ltree_gist'::pg_catalog.regtype
WHERE oid = pg_catalog.to_regprocedure('_ltree_union(internal,internal)');
