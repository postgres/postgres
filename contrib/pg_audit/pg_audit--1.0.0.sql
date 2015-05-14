/* pg_audit/pg_audit--1.0.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_audit" to load this file.\quit

CREATE FUNCTION pg_audit_ddl_command_end()
	RETURNS event_trigger
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'pg_audit_ddl_command_end';

CREATE EVENT TRIGGER pg_audit_ddl_command_end
	ON ddl_command_end
	EXECUTE PROCEDURE pg_audit_ddl_command_end();

CREATE FUNCTION pg_audit_sql_drop()
	RETURNS event_trigger
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'pg_audit_sql_drop';

CREATE EVENT TRIGGER pg_audit_sql_drop
	ON sql_drop
	EXECUTE PROCEDURE pg_audit_sql_drop();
