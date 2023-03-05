-- ok, superuser can create users with any set of privileges
CREATE ROLE regress_role_super SUPERUSER;
CREATE ROLE regress_role_admin CREATEDB CREATEROLE REPLICATION BYPASSRLS;
GRANT CREATE ON DATABASE regression TO regress_role_admin WITH GRANT OPTION;
CREATE ROLE regress_role_limited_admin CREATEROLE;
CREATE ROLE regress_role_normal;

-- fail, CREATEROLE user can't give away role attributes without having them
SET SESSION AUTHORIZATION regress_role_limited_admin;
CREATE ROLE regress_nosuch_superuser SUPERUSER;
CREATE ROLE regress_nosuch_replication_bypassrls REPLICATION BYPASSRLS;
CREATE ROLE regress_nosuch_replication REPLICATION;
CREATE ROLE regress_nosuch_bypassrls BYPASSRLS;
CREATE ROLE regress_nosuch_createdb CREATEDB;

-- ok, can create a role without any special attributes
CREATE ROLE regress_role_limited;

-- fail, can't give it in any of the restricted attributes
ALTER ROLE regress_role_limited SUPERUSER;
ALTER ROLE regress_role_limited REPLICATION;
ALTER ROLE regress_role_limited CREATEDB;
ALTER ROLE regress_role_limited BYPASSRLS;
DROP ROLE regress_role_limited;

-- ok, can give away these role attributes if you have them
SET SESSION AUTHORIZATION regress_role_admin;
CREATE ROLE regress_replication_bypassrls REPLICATION BYPASSRLS;
CREATE ROLE regress_replication REPLICATION;
CREATE ROLE regress_bypassrls BYPASSRLS;
CREATE ROLE regress_createdb CREATEDB;

-- ok, can toggle these role attributes off and on if you have them
ALTER ROLE regress_replication NOREPLICATION;
ALTER ROLE regress_replication REPLICATION;
ALTER ROLE regress_bypassrls NOBYPASSRLS;
ALTER ROLE regress_bypassrls BYPASSRLS;
ALTER ROLE regress_createdb NOCREATEDB;
ALTER ROLE regress_createdb CREATEDB;

-- fail, can't toggle SUPERUSER
ALTER ROLE regress_createdb SUPERUSER;
ALTER ROLE regress_createdb NOSUPERUSER;

-- ok, having CREATEROLE is enough to create users with these privileges
CREATE ROLE regress_createrole CREATEROLE NOINHERIT;
GRANT CREATE ON DATABASE regression TO regress_createrole WITH GRANT OPTION;
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

-- ok, roles with CREATEROLE can create new roles with different role
-- attributes, including CREATEROLE
CREATE ROLE regress_hasprivs CREATEROLE LOGIN INHERIT CONNECTION LIMIT 5;

-- ok, we should be able to modify a role we created
COMMENT ON ROLE regress_hasprivs IS 'some comment';
ALTER ROLE regress_hasprivs RENAME TO regress_tenant;
ALTER ROLE regress_tenant NOINHERIT NOLOGIN CONNECTION LIMIT 7;

-- fail, we should be unable to modify a role we did not create
COMMENT ON ROLE regress_role_normal IS 'some comment';
ALTER ROLE regress_role_normal RENAME TO regress_role_abnormal;
ALTER ROLE regress_role_normal NOINHERIT NOLOGIN CONNECTION LIMIT 7;

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

-- fail, can't create objects owned as regress_tenant
CREATE SCHEMA regress_tenant_schema AUTHORIZATION regress_tenant;

-- fail, we don't inherit permissions from regress_tenant
REASSIGN OWNED BY regress_tenant TO regress_createrole;

-- ok, create a role with a value for createrole_self_grant
SET createrole_self_grant = 'set, inherit';
CREATE ROLE regress_tenant2;
GRANT CREATE ON DATABASE regression TO regress_tenant2;

-- ok, regress_tenant2 can create objects within the database
SET SESSION AUTHORIZATION regress_tenant2;
CREATE TABLE tenant2_table (i integer);
REVOKE ALL PRIVILEGES ON tenant2_table FROM PUBLIC;

-- ok, because we have SET and INHERIT on regress_tenant2
SET SESSION AUTHORIZATION regress_createrole;
CREATE SCHEMA regress_tenant2_schema AUTHORIZATION regress_tenant2;
ALTER SCHEMA regress_tenant2_schema OWNER TO regress_createrole;
ALTER TABLE tenant2_table OWNER TO regress_createrole;
ALTER TABLE tenant2_table OWNER TO regress_tenant2;

-- with SET but not INHERIT, we can give away objects but not take them
REVOKE INHERIT OPTION FOR regress_tenant2 FROM regress_createrole;
ALTER SCHEMA regress_tenant2_schema OWNER TO regress_tenant2;
ALTER TABLE tenant2_table OWNER TO regress_createrole;

-- with INHERIT but not SET, we can take objects but not give them away
GRANT regress_tenant2 TO regress_createrole WITH INHERIT TRUE, SET FALSE;
ALTER TABLE tenant2_table OWNER TO regress_createrole;
ALTER TABLE tenant2_table OWNER TO regress_tenant2;
DROP TABLE tenant2_table;

-- fail, CREATEROLE is not enough to create roles in privileged roles
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

-- fail, role still owns database objects
DROP ROLE regress_tenant;

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

-- must revoke privileges before dropping role
REVOKE CREATE ON DATABASE regression FROM regress_createrole CASCADE;

-- ok, should be able to drop non-superuser roles we created
DROP ROLE regress_replication_bypassrls;
DROP ROLE regress_replication;
DROP ROLE regress_bypassrls;
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

-- fail, cannot drop ourself, nor superusers or roles we lack ADMIN for
DROP ROLE regress_role_super;
DROP ROLE regress_role_admin;
DROP ROLE regress_rolecreator;

-- ok
RESET SESSION AUTHORIZATION;
REVOKE CREATE ON DATABASE regression FROM regress_role_admin CASCADE;
DROP INDEX tenant_idx;
DROP TABLE tenant_table;
DROP VIEW tenant_view;
DROP SCHEMA regress_tenant2_schema;
DROP ROLE regress_tenant;
DROP ROLE regress_tenant2;
DROP ROLE regress_rolecreator;
DROP ROLE regress_role_admin;
DROP ROLE regress_role_limited_admin;
DROP ROLE regress_role_super;
DROP ROLE regress_role_normal;
