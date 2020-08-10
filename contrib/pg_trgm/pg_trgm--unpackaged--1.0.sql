/* contrib/pg_trgm/pg_trgm--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_trgm FROM unpackaged" to load this file. \quit

ALTER EXTENSION pg_trgm ADD function set_limit(real);
ALTER EXTENSION pg_trgm ADD function show_limit();
ALTER EXTENSION pg_trgm ADD function show_trgm(text);
ALTER EXTENSION pg_trgm ADD function similarity(text,text);
ALTER EXTENSION pg_trgm ADD function similarity_op(text,text);
ALTER EXTENSION pg_trgm ADD operator %(text,text);
ALTER EXTENSION pg_trgm ADD type gtrgm;
ALTER EXTENSION pg_trgm ADD function gtrgm_in(cstring);
ALTER EXTENSION pg_trgm ADD function gtrgm_out(gtrgm);
ALTER EXTENSION pg_trgm ADD function gtrgm_consistent(internal,text,integer,oid,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_compress(internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_decompress(internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_penalty(internal,internal,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_picksplit(internal,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_union(bytea,internal);
ALTER EXTENSION pg_trgm ADD function gtrgm_same(gtrgm,gtrgm,internal);
ALTER EXTENSION pg_trgm ADD operator family gist_trgm_ops using gist;
ALTER EXTENSION pg_trgm ADD operator class gist_trgm_ops using gist;
ALTER EXTENSION pg_trgm ADD operator family gin_trgm_ops using gin;
ALTER EXTENSION pg_trgm ADD operator class gin_trgm_ops using gin;

-- These functions had different names/signatures in 9.0.  We can't just
-- drop and recreate them because they are linked into the GIN opclass,
-- so we need some ugly hacks.

-- First, absorb them into the extension under their old names.

ALTER EXTENSION pg_trgm ADD function gin_extract_trgm(text, internal);
ALTER EXTENSION pg_trgm ADD function gin_extract_trgm(text, internal, int2, internal, internal);
ALTER EXTENSION pg_trgm ADD function gin_trgm_consistent(internal,smallint,text,integer,internal,internal);

-- Fix the names, and then do CREATE OR REPLACE to adjust the function
-- bodies to be correct (ie, reference the correct C symbol).

ALTER FUNCTION gin_extract_trgm(text, internal)
  RENAME TO gin_extract_value_trgm;
CREATE OR REPLACE FUNCTION gin_extract_value_trgm(text, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

ALTER FUNCTION gin_extract_trgm(text, internal, int2, internal, internal)
  RENAME TO gin_extract_query_trgm;
CREATE OR REPLACE FUNCTION gin_extract_query_trgm(text, internal, int2, internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- gin_trgm_consistent didn't change name.

-- Last, fix the parameter lists by means of direct UPDATE on the pg_proc
-- entries.  This is ugly as can be, but there's no other way to do it
-- while preserving the identities (OIDs) of the functions.

DO LANGUAGE plpgsql
$$
DECLARE
  my_schema pg_catalog.text := pg_catalog.quote_ident(pg_catalog.current_schema());
  old_path pg_catalog.text := pg_catalog.current_setting('search_path');
BEGIN
-- for safety, transiently set search_path to just pg_catalog+pg_temp
PERFORM pg_catalog.set_config('search_path', 'pg_catalog, pg_temp', true);

UPDATE pg_catalog.pg_proc
SET pronargs = 7, proargtypes = '25 2281 21 2281 2281 2281 2281'
WHERE oid = (my_schema || '.gin_extract_query_trgm(text,internal,int2,internal,internal)')::pg_catalog.regprocedure;

UPDATE pg_catalog.pg_proc
SET pronargs = 8, proargtypes = '2281 21 25 23 2281 2281 2281 2281'
WHERE oid = (my_schema || '.gin_trgm_consistent(internal,smallint,text,integer,internal,internal)')::pg_catalog.regprocedure;

PERFORM pg_catalog.set_config('search_path', old_path, true);
END
$$;


-- These were not in 9.0:

CREATE FUNCTION similarity_dist(text,text)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE OPERATOR <-> (
        LEFTARG = text,
        RIGHTARG = text,
        PROCEDURE = similarity_dist,
        COMMUTATOR = '<->'
);

CREATE FUNCTION gtrgm_distance(internal,text,int,oid)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT;

-- Add new stuff to the operator classes.  See comment in pg_trgm--1.0.sql.

ALTER OPERATOR FAMILY gist_trgm_ops USING gist ADD
        OPERATOR        2       <-> (text, text) FOR ORDER BY pg_catalog.float_ops,
        OPERATOR        3       pg_catalog.~~ (text, text),
        OPERATOR        4       pg_catalog.~~* (text, text),
        FUNCTION        8 (text, text)  gtrgm_distance (internal, text, int, oid);

ALTER OPERATOR FAMILY gin_trgm_ops USING gin ADD
        OPERATOR        3       pg_catalog.~~ (text, text),
        OPERATOR        4       pg_catalog.~~* (text, text);
