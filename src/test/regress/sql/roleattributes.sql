-- default for superuser is false
CREATE ROLE test_def_superuser;
SELECT * FROM pg_authid WHERE rolname = 'test_def_superuser';
CREATE ROLE test_superuser WITH SUPERUSER;
SELECT * FROM pg_authid WHERE rolname = 'test_superuser';
ALTER ROLE test_superuser WITH NOSUPERUSER;
SELECT * FROM pg_authid WHERE rolname = 'test_superuser';
ALTER ROLE test_superuser WITH SUPERUSER;
SELECT * FROM pg_authid WHERE rolname = 'test_superuser';

-- default for inherit is true
CREATE ROLE test_def_inherit;
SELECT * FROM pg_authid WHERE rolname = 'test_def_inherit';
CREATE ROLE test_inherit WITH NOINHERIT;
SELECT * FROM pg_authid WHERE rolname = 'test_inherit';
ALTER ROLE test_inherit WITH INHERIT;
SELECT * FROM pg_authid WHERE rolname = 'test_inherit';
ALTER ROLE test_inherit WITH NOINHERIT;
SELECT * FROM pg_authid WHERE rolname = 'test_inherit';

-- default for create role is false
CREATE ROLE test_def_createrole;
SELECT * FROM pg_authid WHERE rolname = 'test_def_createrole';
CREATE ROLE test_createrole WITH CREATEROLE;
SELECT * FROM pg_authid WHERE rolname = 'test_createrole';
ALTER ROLE test_createrole WITH NOCREATEROLE;
SELECT * FROM pg_authid WHERE rolname = 'test_createrole';
ALTER ROLE test_createrole WITH CREATEROLE;
SELECT * FROM pg_authid WHERE rolname = 'test_createrole';

-- default for create database is false
CREATE ROLE test_def_createdb;
SELECT * FROM pg_authid WHERE rolname = 'test_def_createdb';
CREATE ROLE test_createdb WITH CREATEDB;
SELECT * FROM pg_authid WHERE rolname = 'test_createdb';
ALTER ROLE test_createdb WITH NOCREATEDB;
SELECT * FROM pg_authid WHERE rolname = 'test_createdb';
ALTER ROLE test_createdb WITH CREATEDB;
SELECT * FROM pg_authid WHERE rolname = 'test_createdb';

-- default for can login is false for role
CREATE ROLE test_def_role_canlogin;
SELECT * FROM pg_authid WHERE rolname = 'test_def_role_canlogin';
CREATE ROLE test_role_canlogin WITH LOGIN;
SELECT * FROM pg_authid WHERE rolname = 'test_role_canlogin';
ALTER ROLE test_role_canlogin WITH NOLOGIN;
SELECT * FROM pg_authid WHERE rolname = 'test_role_canlogin';
ALTER ROLE test_role_canlogin WITH LOGIN;
SELECT * FROM pg_authid WHERE rolname = 'test_role_canlogin';

-- default for can login is true for user
CREATE USER test_def_user_canlogin;
SELECT * FROM pg_authid WHERE rolname = 'test_def_user_canlogin';
CREATE USER test_user_canlogin WITH NOLOGIN;
SELECT * FROM pg_authid WHERE rolname = 'test_user_canlogin';
ALTER USER test_user_canlogin WITH LOGIN;
SELECT * FROM pg_authid WHERE rolname = 'test_user_canlogin';
ALTER USER test_user_canlogin WITH NOLOGIN;
SELECT * FROM pg_authid WHERE rolname = 'test_user_canlogin';

-- default for replication is false
CREATE ROLE test_def_replication;
SELECT * FROM pg_authid WHERE rolname = 'test_def_replication';
CREATE ROLE test_replication WITH REPLICATION;
SELECT * FROM pg_authid WHERE rolname = 'test_replication';
ALTER ROLE test_replication WITH NOREPLICATION;
SELECT * FROM pg_authid WHERE rolname = 'test_replication';
ALTER ROLE test_replication WITH REPLICATION;
SELECT * FROM pg_authid WHERE rolname = 'test_replication';

-- default for bypassrls is false
CREATE ROLE test_def_bypassrls;
SELECT * FROM pg_authid WHERE rolname = 'test_def_bypassrls';
CREATE ROLE test_bypassrls WITH BYPASSRLS;
SELECT * FROM pg_authid WHERE rolname = 'test_bypassrls';
ALTER ROLE test_bypassrls WITH NOBYPASSRLS;
SELECT * FROM pg_authid WHERE rolname = 'test_bypassrls';
ALTER ROLE test_bypassrls WITH BYPASSRLS;
SELECT * FROM pg_authid WHERE rolname = 'test_bypassrls';

-- clean up roles
DROP ROLE test_def_superuser;
DROP ROLE test_superuser;
DROP ROLE test_def_inherit;
DROP ROLE test_inherit;
DROP ROLE test_def_createrole;
DROP ROLE test_createrole;
DROP ROLE test_def_createdb;
DROP ROLE test_createdb;
DROP ROLE test_def_role_canlogin;
DROP ROLE test_role_canlogin;
DROP USER test_def_user_canlogin;
DROP USER test_user_canlogin;
DROP ROLE test_def_replication;
DROP ROLE test_replication;
DROP ROLE test_def_bypassrls;
DROP ROLE test_bypassrls;
