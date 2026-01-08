/* contrib/btree_gist/btree_gist--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO '1.9'" to load this file. \quit

--
-- Mark gist_inet_ops and gist_cidr_ops opclasses as non-default.
-- This is the first step on the way to eventually removing them.
--
-- There's no SQL command for this, so fake it with a manual update on
-- pg_opclass.
--
DO LANGUAGE plpgsql
$$
DECLARE
  my_schema pg_catalog.text := pg_catalog.quote_ident(pg_catalog.current_schema());
  old_path pg_catalog.text := pg_catalog.current_setting('search_path');
BEGIN
-- for safety, transiently set search_path to just pg_catalog+pg_temp
PERFORM pg_catalog.set_config('search_path', 'pg_catalog, pg_temp', true);

UPDATE pg_catalog.pg_opclass
SET opcdefault = false
WHERE opcmethod = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'gist') AND
      opcname IN ('gist_inet_ops', 'gist_cidr_ops') AND
      opcnamespace = my_schema::pg_catalog.regnamespace;

PERFORM pg_catalog.set_config('search_path', old_path, true);
END
$$;


-- Fix parallel-safety markings overlooked in btree_gist--1.6--1.7.sql.
ALTER FUNCTION gbt_bool_consistent(internal, bool, smallint, oid, internal) PARALLEL SAFE;
ALTER FUNCTION gbt_bool_compress(internal) PARALLEL SAFE;
ALTER FUNCTION gbt_bool_fetch(internal) PARALLEL SAFE;
ALTER FUNCTION gbt_bool_penalty(internal, internal, internal) PARALLEL SAFE;
ALTER FUNCTION gbt_bool_picksplit(internal, internal) PARALLEL SAFE;
ALTER FUNCTION gbt_bool_union(internal, internal) PARALLEL SAFE;
ALTER FUNCTION gbt_bool_same(gbtreekey2, gbtreekey2, internal) PARALLEL SAFE;
