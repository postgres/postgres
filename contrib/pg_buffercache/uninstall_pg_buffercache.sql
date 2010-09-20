/* contrib/pg_buffercache/uninstall_pg_buffercache.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_buffercache;

DROP FUNCTION pg_buffercache_pages();
