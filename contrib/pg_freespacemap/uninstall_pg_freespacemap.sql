/* $PostgreSQL: pgsql/contrib/pg_freespacemap/uninstall_pg_freespacemap.sql,v 1.5 2009/06/10 22:12:28 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION pg_freespace(regclass, bigint);
DROP FUNCTION pg_freespace(regclass);
