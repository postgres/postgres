-- ok, superuser can create users with any set of privileges
CREATE ROLE regress_role_super SUPERUSER;
CREATE ROLE regress_role_admin CREATEDB CREATEROLE REPLICATION BYPASSRLS;

-- fail, only superusers can create users with these privileges
SET SESSION AUTHORIZATION regress_role_admin;
CREATE ROLE regress_nosuch_superuser SUPERUSER;
CREATE ROLE regress_nosuch_replication_bypassrls REPLICATION BYPASSRLS;
CREATE ROLE regress_nosuch_replication REPLICATION;
CREATE ROLE regress_nosuch_bypassrls BYPASSRLS;

-- ok, having CREATEROLE is enough to create users with these privileges
CREATE ROLE regress_createdb CREATEDB;
CREATE ROLE regress_createrole CREATEROLE;
CREATE ROLE regress_login LOGIN;
CREATE ROLE regress_inherit INHERIT;
CREATE ROLE regress_connection_limit CONNECTION LIMIT 5;
CREATE ROLE regress_encrypted_password ENCRYPTED PASSWORD 'foo';
CREATE ROLE regress_password_null PASSWORD NULL;

-- ok, backwards compatible noise words should be ignored
CREATE ROLE regress_noiseword SYSID 12345;

-- fail, cannot grant membership in superuser role
CREATE ROLE regress_nosuch_super IN ROLE regress_role_super;

-- fail, database owner cannot have members
CREATE ROLE regress_nosuch_dbowner IN ROLE pg_database_owner;

-- ok, can grant other users into a role
CREATE ROLE regress_inroles ROLE
	regress_role_super, regress_createdb, regress_createrole, regress_login,
	regress_inherit, regress_connection_limit, regress_encrypted_password, regress_password_null;

-- fail, cannot grant a role into itself
CREATE ROLE regress_nosuch_recursive ROLE regress_nosuch_recursive;

-- ok, can grant other users into a role with admin option
CREATE ROLE regress_adminroles ADMIN
	regress_role_super, regress_createdb, regress_createrole, regress_login,
	regress_inherit, regress_connection_limit, regress_encrypted_password, regress_password_null;

-- fail, cannot grant a role into itself with admin option
CREATE ROLE regress_nosuch_admin_recursive ADMIN regress_nosuch_admin_recursive;

-- fail, regress_createrole does not have CREATEDB privilege
SET SESSION AUTHORIZATION regress_createrole;
CREATE DATABASE regress_nosuch_db;

-- ok, regress_createrole can create new roles
CREATE ROLE regress_plainrole;

-- ok, roles with CREATEROLE can create new roles with it
CREATE ROLE regress_rolecreator CREATEROLE;

-- ok, roles with CREATEROLE can create new roles with privilege they lack
CREATE ROLE regress_tenant CREATEDB CREATEROLE LOGIN INHERIT CONNECTION LIMIT 5;

-- ok, regress_tenant can create objects within the database
SET SESSION AUTHORIZATION regress_tenant;
CREATE TABLE tenant_table (i integer);
CREATE INDEX tenant_idx ON tenant_table(i);
CREATE VIEW tenant_view AS SELECT * FROM pg_catalog.pg_class;
REVOKE ALL PRIVILEGES ON tenant_table FROM PUBLIC;

-- fail, these objects belonging to regress_tenant
SET SESSION AUTHORIZATION regress_createrole;
DROP INDEX tenant_idx;
ALTER TABLE tenant_table ADD COLUMN t text;
DROP TABLE tenant_table;
ALTER VIEW tenant_view OWNER TO regress_role_admin;
DROP VIEW tenant_view;

-- fail, cannot take ownership of these objects from regress_tenant
REASSIGN OWNED BY regress_tenant TO regress_createrole;

-- ok, having CREATEROLE is enough to create roles in privileged roles
CREATE ROLE regress_read_all_data IN ROLE pg_read_all_data;
CREATE ROLE regress_write_all_data IN ROLE pg_write_all_data;
CREATE ROLE regress_monitor IN ROLE pg_monitor;
CREATE ROLE regress_read_all_settings IN ROLE pg_read_all_settings;
CREATE ROLE regress_read_all_stats IN ROLE pg_read_all_stats;
CREATE ROLE regress_stat_scan_tables IN ROLE pg_stat_scan_tables;
CREATE ROLE regress_read_server_files IN ROLE pg_read_server_files;
CREATE ROLE regress_write_server_files IN ROLE pg_write_server_files;
CREATE ROLE regress_execute_server_program IN ROLE pg_execute_server_program;
CREATE ROLE regress_signal_backend IN ROLE pg_signal_backend;

-- fail, creation of these roles failed above so they do not now exist
SET SESSION AUTHORIZATION regress_role_admin;
DROP ROLE regress_nosuch_superuser;
DROP ROLE regress_nosuch_replication_bypassrls;
DROP ROLE regress_nosuch_replication;
DROP ROLE regress_nosuch_bypassrls;
DROP ROLE regress_nosuch_super;
DROP ROLE regress_nosuch_dbowner;
DROP ROLE regress_nosuch_recursive;
DROP ROLE regress_nosuch_admin_recursive;
DROP ROLE regress_plainrole;

-- ok, should be able to drop non-superuser roles we created
DROP ROLE regress_createdb;
DROP ROLE regress_createrole;
DROP ROLE regress_login;
DROP ROLE regress_inherit;
DROP ROLE regress_connection_limit;
DROP ROLE regress_encrypted_password;
DROP ROLE regress_password_null;
DROP ROLE regress_noiseword;
DROP ROLE regress_inroles;
DROP ROLE regress_adminroles;
DROP ROLE regress_rolecreator;
DROP ROLE regress_read_all_data;
DROP ROLE regress_write_all_data;
DROP ROLE regress_monitor;
DROP ROLE regress_read_all_settings;
DROP ROLE regress_read_all_stats;
DROP ROLE regress_stat_scan_tables;
DROP ROLE regress_read_server_files;
DROP ROLE regress_write_server_files;
DROP ROLE regress_execute_server_program;
DROP ROLE regress_signal_backend;

-- fail, role still owns database objects
DROP ROLE regress_tenant;

-- fail, cannot drop ourself nor superusers
DROP ROLE regress_role_super;
DROP ROLE regress_role_admin;

-- ok
RESET SESSION AUTHORIZATION;
DROP INDEX tenant_idx;
DROP TABLE tenant_table;
DROP VIEW tenant_view;
DROP ROLE regress_tenant;
DROP ROLE regress_role_admin;
DROP ROLE regress_role_super;
