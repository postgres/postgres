/* contrib/intarray/intarray--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION intarray FROM unpackaged" to load this file. \quit

ALTER EXTENSION intarray ADD type query_int;
ALTER EXTENSION intarray ADD function bqarr_in(cstring);
ALTER EXTENSION intarray ADD function bqarr_out(query_int);
ALTER EXTENSION intarray ADD function querytree(query_int);
ALTER EXTENSION intarray ADD function boolop(integer[],query_int);
ALTER EXTENSION intarray ADD function rboolop(query_int,integer[]);
ALTER EXTENSION intarray ADD operator ~~(query_int,integer[]);
ALTER EXTENSION intarray ADD operator @@(integer[],query_int);
ALTER EXTENSION intarray ADD function _int_contains(integer[],integer[]);
ALTER EXTENSION intarray ADD function _int_contained(integer[],integer[]);
ALTER EXTENSION intarray ADD function _int_overlap(integer[],integer[]);
ALTER EXTENSION intarray ADD function _int_same(integer[],integer[]);
ALTER EXTENSION intarray ADD function _int_different(integer[],integer[]);
ALTER EXTENSION intarray ADD function _int_union(integer[],integer[]);
ALTER EXTENSION intarray ADD function _int_inter(integer[],integer[]);
ALTER EXTENSION intarray ADD operator &&(integer[],integer[]);
ALTER EXTENSION intarray ADD operator <@(integer[],integer[]);
ALTER EXTENSION intarray ADD operator @>(integer[],integer[]);
ALTER EXTENSION intarray ADD operator ~(integer[],integer[]);
ALTER EXTENSION intarray ADD operator @(integer[],integer[]);
ALTER EXTENSION intarray ADD function intset(integer);
ALTER EXTENSION intarray ADD function icount(integer[]);
ALTER EXTENSION intarray ADD operator #(NONE,integer[]);
ALTER EXTENSION intarray ADD function sort(integer[],text);
ALTER EXTENSION intarray ADD function sort(integer[]);
ALTER EXTENSION intarray ADD function sort_asc(integer[]);
ALTER EXTENSION intarray ADD function sort_desc(integer[]);
ALTER EXTENSION intarray ADD function uniq(integer[]);
ALTER EXTENSION intarray ADD function idx(integer[],integer);
ALTER EXTENSION intarray ADD operator #(integer[],integer);
ALTER EXTENSION intarray ADD function subarray(integer[],integer,integer);
ALTER EXTENSION intarray ADD function subarray(integer[],integer);
ALTER EXTENSION intarray ADD function intarray_push_elem(integer[],integer);
ALTER EXTENSION intarray ADD operator +(integer[],integer);
ALTER EXTENSION intarray ADD function intarray_push_array(integer[],integer[]);
ALTER EXTENSION intarray ADD operator +(integer[],integer[]);
ALTER EXTENSION intarray ADD function intarray_del_elem(integer[],integer);
ALTER EXTENSION intarray ADD operator -(integer[],integer);
ALTER EXTENSION intarray ADD function intset_union_elem(integer[],integer);
ALTER EXTENSION intarray ADD operator |(integer[],integer);
ALTER EXTENSION intarray ADD operator |(integer[],integer[]);
ALTER EXTENSION intarray ADD function intset_subtract(integer[],integer[]);
ALTER EXTENSION intarray ADD operator -(integer[],integer[]);
ALTER EXTENSION intarray ADD operator &(integer[],integer[]);
ALTER EXTENSION intarray ADD function g_int_consistent(internal,integer[],integer,oid,internal);
ALTER EXTENSION intarray ADD function g_int_compress(internal);
ALTER EXTENSION intarray ADD function g_int_decompress(internal);
ALTER EXTENSION intarray ADD function g_int_penalty(internal,internal,internal);
ALTER EXTENSION intarray ADD function g_int_picksplit(internal,internal);
ALTER EXTENSION intarray ADD function g_int_union(internal,internal);
ALTER EXTENSION intarray ADD function g_int_same(integer[],integer[],internal);
ALTER EXTENSION intarray ADD operator family gist__int_ops using gist;
ALTER EXTENSION intarray ADD operator class gist__int_ops using gist;
ALTER EXTENSION intarray ADD type intbig_gkey;
ALTER EXTENSION intarray ADD function _intbig_in(cstring);
ALTER EXTENSION intarray ADD function _intbig_out(intbig_gkey);
ALTER EXTENSION intarray ADD function g_intbig_consistent(internal,internal,integer,oid,internal);
ALTER EXTENSION intarray ADD function g_intbig_compress(internal);
ALTER EXTENSION intarray ADD function g_intbig_decompress(internal);
ALTER EXTENSION intarray ADD function g_intbig_penalty(internal,internal,internal);
ALTER EXTENSION intarray ADD function g_intbig_picksplit(internal,internal);
ALTER EXTENSION intarray ADD function g_intbig_union(internal,internal);
ALTER EXTENSION intarray ADD function g_intbig_same(internal,internal,internal);
ALTER EXTENSION intarray ADD operator family gist__intbig_ops using gist;
ALTER EXTENSION intarray ADD operator class gist__intbig_ops using gist;
ALTER EXTENSION intarray ADD operator family gin__int_ops using gin;
ALTER EXTENSION intarray ADD operator class gin__int_ops using gin;

-- These functions had different signatures in 9.0.  We can't just
-- drop and recreate them because they are linked into the GIN opclass,
-- so we need some ugly hacks.

-- First, absorb them into the extension under their old identities.

ALTER EXTENSION intarray ADD function ginint4_queryextract(internal,internal,smallint,internal,internal);
ALTER EXTENSION intarray ADD function ginint4_consistent(internal,smallint,internal,integer,internal,internal);

-- Next, fix the parameter lists by means of direct UPDATE on the pg_proc
-- entries.  This is ugly as can be, but there's no other way to do it
-- while preserving the identities (OIDs) of the functions.

UPDATE pg_catalog.pg_proc
SET pronargs = 7, proargtypes = '2281 2281 21 2281 2281 2281 2281'
WHERE oid = 'ginint4_queryextract(internal,internal,smallint,internal,internal)'::pg_catalog.regprocedure;

UPDATE pg_catalog.pg_proc
SET pronargs = 8, proargtypes = '2281 21 2281 23 2281 2281 2281 2281'
WHERE oid = 'ginint4_consistent(internal,smallint,internal,integer,internal,internal)'::pg_catalog.regprocedure;

-- intarray also relies on the core function ginarrayextract, which changed
-- signature in 9.1.  To support upgrading, pg_catalog contains entries
-- for ginarrayextract with both 2 and 3 args, and the former is what would
-- have been added to our opclass during initial restore of a 9.0 dump script.
-- Avert your eyes while we hack the pg_amproc entry to make it link to the
-- 3-arg form ...

UPDATE pg_catalog.pg_amproc
SET amproc = 'pg_catalog.ginarrayextract(anyarray,internal,internal)'::pg_catalog.regprocedure
WHERE amprocfamily =
  (SELECT oid FROM pg_catalog.pg_opfamily WHERE opfname = 'gin__int_ops' AND
     opfnamespace = (SELECT oid FROM pg_catalog.pg_namespace
                     WHERE nspname = pg_catalog.current_schema()))
  AND amproclefttype = 'integer[]'::pg_catalog.regtype
  AND amprocrighttype = 'integer[]'::pg_catalog.regtype
  AND amprocnum = 2
  AND amproc = 'pg_catalog.ginarrayextract(anyarray,internal)'::pg_catalog.regprocedure;
