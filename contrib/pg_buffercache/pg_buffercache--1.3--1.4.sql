/* contrib/pg_buffercache/pg_buffercache--1.3--1.4.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.4'" to load this file. \quit

/* First we have to remove them from the extension */
ALTER EXTENSION pg_buffercache DROP VIEW pg_buffercache;
ALTER EXTENSION pg_buffercache DROP FUNCTION pg_buffercache_pages();

/* Then we can drop them */
DROP VIEW pg_buffercache;
DROP FUNCTION pg_buffercache_pages();

/* Now redefine */
CREATE FUNCTION pg_buffercache_pages()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_buffercache_pages_v1_4'
LANGUAGE C PARALLEL SAFE;

CREATE VIEW pg_buffercache AS
	SELECT P.* FROM pg_buffercache_pages() AS P
	(bufferid integer, relfilenode int8, reltablespace oid, reldatabase oid,
	 relforknumber int2, relblocknumber int8, isdirty bool, usagecount int2,
	 pinning_backends int4);

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION pg_buffercache_pages() FROM PUBLIC;
REVOKE ALL ON pg_buffercache FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_buffercache_pages() TO pg_monitor;
GRANT SELECT ON pg_buffercache TO pg_monitor;
