/* contrib/pg_stat_statements/uninstall_pg_stat_statements.sql */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP VIEW pg_stat_statements;
DROP FUNCTION pg_stat_statements();
DROP FUNCTION pg_stat_statements_reset();
