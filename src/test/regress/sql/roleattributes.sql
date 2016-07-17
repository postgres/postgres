-- default for superuser is false
CREATE ROLE regress_test_def_superuser;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_superuser';
CREATE ROLE regress_test_superuser WITH SUPERUSER;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_superuser';
ALTER ROLE regress_test_superuser WITH NOSUPERUSER;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_superuser';
ALTER ROLE regress_test_superuser WITH SUPERUSER;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_superuser';

-- default for inherit is true
CREATE ROLE regress_test_def_inherit;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_inherit';
CREATE ROLE regress_test_inherit WITH NOINHERIT;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_inherit';
ALTER ROLE regress_test_inherit WITH INHERIT;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_inherit';
ALTER ROLE regress_test_inherit WITH NOINHERIT;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_inherit';

-- default for create role is false
CREATE ROLE regress_test_def_createrole;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_createrole';
CREATE ROLE regress_test_createrole WITH CREATEROLE;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_createrole';
ALTER ROLE regress_test_createrole WITH NOCREATEROLE;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_createrole';
ALTER ROLE regress_test_createrole WITH CREATEROLE;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_createrole';

-- default for create database is false
CREATE ROLE regress_test_def_createdb;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_createdb';
CREATE ROLE regress_test_createdb WITH CREATEDB;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_createdb';
ALTER ROLE regress_test_createdb WITH NOCREATEDB;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_createdb';
ALTER ROLE regress_test_createdb WITH CREATEDB;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_createdb';

-- default for can login is false for role
CREATE ROLE regress_test_def_role_canlogin;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_role_canlogin';
CREATE ROLE regress_test_role_canlogin WITH LOGIN;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_role_canlogin';
ALTER ROLE regress_test_role_canlogin WITH NOLOGIN;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_role_canlogin';
ALTER ROLE regress_test_role_canlogin WITH LOGIN;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_role_canlogin';

-- default for can login is true for user
CREATE USER regress_test_def_user_canlogin;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_user_canlogin';
CREATE USER regress_test_user_canlogin WITH NOLOGIN;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_user_canlogin';
ALTER USER regress_test_user_canlogin WITH LOGIN;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_user_canlogin';
ALTER USER regress_test_user_canlogin WITH NOLOGIN;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_user_canlogin';

-- default for replication is false
CREATE ROLE regress_test_def_replication;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_replication';
CREATE ROLE regress_test_replication WITH REPLICATION;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_replication';
ALTER ROLE regress_test_replication WITH NOREPLICATION;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_replication';
ALTER ROLE regress_test_replication WITH REPLICATION;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_replication';

-- default for bypassrls is false
CREATE ROLE regress_test_def_bypassrls;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_def_bypassrls';
CREATE ROLE regress_test_bypassrls WITH BYPASSRLS;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_bypassrls';
ALTER ROLE regress_test_bypassrls WITH NOBYPASSRLS;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_bypassrls';
ALTER ROLE regress_test_bypassrls WITH BYPASSRLS;
SELECT * FROM pg_authid WHERE rolname = 'regress_test_bypassrls';

-- clean up roles
DROP ROLE regress_test_def_superuser;
DROP ROLE regress_test_superuser;
DROP ROLE regress_test_def_inherit;
DROP ROLE regress_test_inherit;
DROP ROLE regress_test_def_createrole;
DROP ROLE regress_test_createrole;
DROP ROLE regress_test_def_createdb;
DROP ROLE regress_test_createdb;
DROP ROLE regress_test_def_role_canlogin;
DROP ROLE regress_test_role_canlogin;
DROP USER regress_test_def_user_canlogin;
DROP USER regress_test_user_canlogin;
DROP ROLE regress_test_def_replication;
DROP ROLE regress_test_replication;
DROP ROLE regress_test_def_bypassrls;
DROP ROLE regress_test_bypassrls;
