/* $PostgreSQL: pgsql/contrib/pg_stat_statements/uninstall_pg_stat_statements.sql,v 1.1 2009/01/04 22:19:59 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_stat_statements;
DROP FUNCTION pg_stat_statements();
DROP FUNCTION pg_stat_statements_reset();
