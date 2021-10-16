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

CREATE DATABASE role_membership_test1;
CREATE DATABASE role_membership_test2;
CREATE DATABASE role_membership_test3;
CREATE DATABASE role_membership_test4;

-- Initial GRANT statements

GRANT pg_read_all_data TO role_a WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test2;

\connect role_membership_test3 role_admin

GRANT pg_read_all_data TO role_b IN CURRENT DATABASE;
GRANT pg_read_all_data TO role_c IN CURRENT DATABASE WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_c IN DATABASE role_membership_test4 GRANTED BY role_a;

\connect postgres role_admin

SELECT * FROM check_memberships();

-- Ensure GRANT warning messages for duplicate grants
GRANT pg_read_all_data TO role_a;
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test3;

-- Ensure with admin option can still be granted without warning
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test2 WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_b IN DATABASE role_membership_test2 WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_c IN DATABASE role_membership_test3 WITH ADMIN OPTION;

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
-- test drop database cleanup

-- should deny database-specific grants for superuser roles?

SELECT * FROM check_memberships();
