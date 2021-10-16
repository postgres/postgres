CREATE ROLE role_admin LOGIN SUPERUSER;

\connect postgres role_admin

CREATE FUNCTION check_memberships()
 RETURNS TABLE (role name, member name, grantor name, admin_option boolean, datname name)
 AS $$
SELECT
  r.rolname as role,
  m.rolname as member,
  g.rolname as grantor,
  admin_option,
  d.datname
FROM pg_auth_members a
LEFT JOIN pg_roles r ON r.oid = a.roleid
LEFT JOIN pg_roles m ON m.oid = a.member
LEFT JOIN pg_roles g ON g.oid = a.grantor
LEFT JOIN pg_database d ON d.oid = a.dbid
WHERE
  m.rolname LIKE 'role__'
ORDER BY
  1, 2, 5
$$ LANGUAGE SQL;

CREATE ROLE role_a;
CREATE ROLE role_b;
CREATE ROLE role_c;
CREATE ROLE role_d;
CREATE ROLE role_e NOINHERIT;

\connect template1

CREATE TABLE data AS SELECT generate_series(1, 3);

CREATE DATABASE role_membership_test1;
CREATE DATABASE role_membership_test2;
CREATE DATABASE role_membership_test3;
CREATE DATABASE role_membership_test4;

-- Initial GRANT statements

GRANT pg_read_all_data TO role_a WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test1;
GRANT role_a TO role_d IN DATABASE role_membership_test1;
GRANT role_a TO role_e;
GRANT role_a TO role_e IN DATABASE role_membership_test1;

\connect role_membership_test2 role_admin

GRANT pg_read_all_data TO role_b IN CURRENT DATABASE;
GRANT pg_read_all_data TO role_c IN CURRENT DATABASE WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_c IN DATABASE role_membership_test4 GRANTED BY role_a;

\connect postgres role_admin

SELECT * FROM check_memberships();

-- Ensure GRANT warning messages for duplicate grants
GRANT pg_read_all_data TO role_a; -- warn
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test2; -- warn

-- Ensure with admin option can still be granted without warning
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test3 WITH ADMIN OPTION; -- silent
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test3 WITH ADMIN OPTION; -- warn
GRANT pg_read_all_data TO role_d IN DATABASE role_membership_test4 WITH ADMIN OPTION; -- silent

SELECT * FROM check_memberships();

-- Test membership privileges
\connect role_membership_test1
SET ROLE role_a;
SELECT * FROM data; -- success (read-all cluster-wide)
SET ROLE role_b;
SELECT * FROM data; -- success (read-all in database)
SET ROLE role_c;
SELECT * FROM data; -- error (not granted)
SET ROLE role_d;
SELECT * FROM data; -- success (inherited from role_a)
SET ROLE role_e;
SELECT * FROM data; -- error (no inherit)

\connect role_membership_test2;
SET ROLE role_a;
SELECT * FROM data; -- success (read-all cluster-wide)
SET ROLE role_b;
SELECT * FROM data; -- success (read-all in database)
SET ROLE role_c;
SELECT * FROM data; -- success (read-all in database)
SET ROLE role_d;
SELECT * FROM data; -- error (not granted)
SET ROLE role_e;
SELECT * FROM data; -- error (no inherit)

-- Ensure ADMIN OPTION can grant cluster-wide and within any DB if cluster-wide
-- Ensure ADMIN OPTION can grant only within same db if database-specific


-- test GRANT works
-- test REVOKE works
-- test grant error (pre-existing)
-- test revoke error (non-existing)
-- test adding admin option
-- test removing admin option

-- test using admin option
-- test set session authorization
-- test set session role

-- test membership privileges

-- Ensure that DROP DATABASE cleans up the relevant memberships

\connect postgres role_admin

DROP DATABASE role_membership_test3;

SELECT * FROM check_memberships();

-- should deny database-specific grants for superuser roles?
