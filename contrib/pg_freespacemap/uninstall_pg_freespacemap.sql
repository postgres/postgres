/* $PostgreSQL: pgsql/contrib/pg_freespacemap/uninstall_pg_freespacemap.sql,v 1.4 2008/09/30 11:17:07 heikki Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_freespacemap_pages;
DROP VIEW pg_freespacemap_relations;

DROP FUNCTION pg_freespacemap_pages();
DROP FUNCTION pg_freespacemap_relations();
