/* $PostgreSQL: pgsql/contrib/pg_freespacemap/uninstall_pg_freespacemap.sql,v 1.3 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_freespacemap_pages;
DROP VIEW pg_freespacemap_relations;

DROP FUNCTION pg_freespacemap_pages();
DROP FUNCTION pg_freespacemap_relations();
