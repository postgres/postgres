/* $PostgreSQL: pgsql/contrib/pgrowlocks/uninstall_pgrowlocks.sql,v 1.4 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION pgrowlocks(text);
