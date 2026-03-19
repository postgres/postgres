-- Consistent test results
SET timezone TO 'UTC';
SET DateStyle TO 'ISO, YMD';

-- Create test database
CREATE DATABASE regression_role_ddl_test;

-- Basic role
CREATE ROLE regress_role_ddl_test1;
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test1');

-- Role with LOGIN
CREATE ROLE regress_role_ddl_test2 LOGIN;
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test2');

-- Role with multiple privileges
CREATE ROLE regress_role_ddl_test3
  LOGIN
  SUPERUSER
  CREATEDB
  CREATEROLE
  CONNECTION LIMIT 5
  VALID UNTIL '2030-12-31 23:59:59+00';
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test3');

-- Role with configuration parameters
CREATE ROLE regress_role_ddl_test4;
ALTER ROLE regress_role_ddl_test4 SET work_mem TO '256MB';
ALTER ROLE regress_role_ddl_test4 SET search_path TO myschema, public;
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test4');

-- Role with database-specific configuration
CREATE ROLE regress_role_ddl_test5;
ALTER ROLE regress_role_ddl_test5 IN DATABASE regression_role_ddl_test SET work_mem TO '128MB';
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test5');

-- Role with special characters (requires quoting)
CREATE ROLE "regress_role-with-dash";
SELECT * FROM pg_get_role_ddl('regress_role-with-dash');

-- Pretty-printed output
\pset format unaligned
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test3', 'pretty', 'true');
\pset format aligned

-- Role with memberships
CREATE ROLE regress_role_ddl_grantor CREATEROLE;
CREATE ROLE regress_role_ddl_group1;
CREATE ROLE regress_role_ddl_group2;
CREATE ROLE regress_role_ddl_member;
GRANT regress_role_ddl_group1 TO regress_role_ddl_grantor WITH ADMIN TRUE;
GRANT regress_role_ddl_group2 TO regress_role_ddl_grantor WITH ADMIN TRUE;
SET ROLE regress_role_ddl_grantor;
GRANT regress_role_ddl_group1 TO regress_role_ddl_member WITH INHERIT TRUE, SET FALSE;
GRANT regress_role_ddl_group2 TO regress_role_ddl_member WITH ADMIN TRUE;
RESET ROLE;
SELECT * FROM pg_get_role_ddl('regress_role_ddl_member');

-- Role with memberships suppressed
SELECT * FROM pg_get_role_ddl('regress_role_ddl_member', 'memberships', 'false');

-- Non-existent role (should error)
SELECT * FROM pg_get_role_ddl(9999999::oid);

-- NULL input (should return no rows)
SELECT * FROM pg_get_role_ddl(NULL);

-- Permission check: revoke SELECT on pg_authid
CREATE ROLE regress_role_ddl_noaccess;
REVOKE SELECT ON pg_authid FROM PUBLIC;
SET ROLE regress_role_ddl_noaccess;
SELECT * FROM pg_get_role_ddl('regress_role_ddl_test1');  -- should fail
RESET ROLE;
GRANT SELECT ON pg_authid TO PUBLIC;
DROP ROLE regress_role_ddl_noaccess;

-- Cleanup
DROP ROLE regress_role_ddl_test1;
DROP ROLE regress_role_ddl_test2;
DROP ROLE regress_role_ddl_test3;
DROP ROLE regress_role_ddl_test4;
DROP ROLE regress_role_ddl_test5;
DROP ROLE "regress_role-with-dash";
SET ROLE regress_role_ddl_grantor;
REVOKE regress_role_ddl_group1 FROM regress_role_ddl_member;
REVOKE regress_role_ddl_group2 FROM regress_role_ddl_member;
RESET ROLE;
DROP ROLE regress_role_ddl_member;
DROP ROLE regress_role_ddl_group1;
DROP ROLE regress_role_ddl_group2;
DROP ROLE regress_role_ddl_grantor;

DROP DATABASE regression_role_ddl_test;

-- Reset timezone to default
RESET timezone;
