/* contrib/hstore/hstore--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION hstore UPDATE TO '1.4'" to load this file. \quit

-- Update procedure signatures the hard way.
-- We use to_regprocedure() so that query doesn't fail if run against 9.6beta1 definitions,
-- wherein the signatures have been updated already.  In that case to_regprocedure() will
-- return NULL and no updates will happen.

UPDATE pg_catalog.pg_proc SET
  proargtypes = pg_catalog.array_to_string(newtypes::pg_catalog.oid[], ' ')::pg_catalog.oidvector,
  pronargs = pg_catalog.array_length(newtypes, 1)
FROM (VALUES
(NULL::pg_catalog.text, NULL::pg_catalog.regtype[]), -- establish column types
('ghstore_same(internal,internal,internal)', '{ghstore,ghstore,internal}'),
('ghstore_consistent(internal,internal,int4,oid,internal)', '{internal,hstore,int2,oid,internal}'),
('gin_extract_hstore(internal,internal)', '{hstore,internal}'),
('gin_extract_hstore_query(internal,internal,int2,internal,internal)', '{hstore,internal,int2,internal,internal}'),
('gin_consistent_hstore(internal,int2,internal,int4,internal,internal)', '{internal,int2,hstore,int4,internal,internal}')
) AS update_data (oldproc, newtypes)
WHERE oid = pg_catalog.to_regprocedure(oldproc);

UPDATE pg_catalog.pg_proc SET
  prorettype = 'ghstore'::pg_catalog.regtype
WHERE oid = pg_catalog.to_regprocedure('ghstore_union(internal,internal)');
