/* $PostgreSQL: pgsql/contrib/pg_buffercache/uninstall_pg_buffercache.sql,v 1.3 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_buffercache;

DROP FUNCTION pg_buffercache_pages();
