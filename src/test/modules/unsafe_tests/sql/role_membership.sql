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

CREATE DATABASE db_1;
CREATE DATABASE db_2;
CREATE DATABASE db_3;
CREATE DATABASE db_4;

-- Initial GRANT statements

GRANT pg_read_all_data TO role_a WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_b IN DATABASE db_1;
GRANT role_a TO role_d IN DATABASE db_1;
GRANT role_a TO role_e;
GRANT role_a TO role_e IN DATABASE db_1;

\connect db_2 role_admin

GRANT pg_read_all_data TO role_b IN CURRENT DATABASE;
GRANT pg_read_all_data TO role_c IN CURRENT DATABASE WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_c IN DATABASE db_4 GRANTED BY role_a;

\connect postgres role_admin

SELECT * FROM check_memberships();

-- Ensure GRANT warning messages for duplicate grants
GRANT pg_read_all_data TO role_a; -- notice
GRANT pg_read_all_data TO role_b IN DATABASE db_2; -- notice

-- Ensure with admin option can still be granted without warning (unless already granted)
GRANT pg_read_all_data TO role_b IN DATABASE db_3 WITH ADMIN OPTION; -- silent
GRANT pg_read_all_data TO role_b IN DATABASE db_3 WITH ADMIN OPTION; -- notice
GRANT pg_read_all_data TO role_b IN DATABASE db_4 WITH ADMIN OPTION; -- silent
GRANT pg_read_all_data TO role_d IN DATABASE db_4 WITH ADMIN OPTION; -- silent

SELECT * FROM check_memberships();

-- Test membership privileges
\connect db_1
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

\connect db_2
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
\connect template1
CREATE ROLE role_f;
CREATE ROLE role_g;
GRANT role_b TO role_g;

-- Test cluster-wide membership
SET ROLE role_a;
GRANT pg_write_all_data TO role_f; -- error (no admin option)
GRANT pg_read_all_data TO role_f; -- success (cluster-wide admin option)
REVOKE pg_read_all_data FROM role_f;
GRANT pg_read_all_data TO role_f IN DATABASE db_4; -- success (cluster-wide admin option)
REVOKE pg_read_all_data FROM role_f IN DATABASE db_4;

-- Ensure ADMIN OPTION grnats are denied if not cluster-wide or if not in the same database when database-specific
SET ROLE role_b;
GRANT pg_read_all_data TO role_f; -- error (no cluster-wide admin option)
GRANT pg_read_all_data TO role_f IN DATABASE db_3; -- error (if admin option is not cluster-wide, database-specific grants are not allowed across databases)

-- Ensure ADMIN OPTION can grant only within same database if database-specific
\connect db_3
SET SESSION AUTHORIZATION role_b;
GRANT pg_read_all_data TO role_f; -- error (no cluster-wide admin option)
GRANT pg_read_all_data TO role_f IN DATABASE db_2; -- error (no admin option for the target database)
GRANT pg_read_all_data TO role_f IN CURRENT DATABASE; -- success (database-specific admin option within the same database)

\connect db_4
SET SESSION AUTHORIZATION role_b;
GRANT pg_read_all_data TO role_f IN DATABASE db_4; -- success (database-specific admin option within the same database)

-- Ensure grant privileges inherit
\connect db_3
SET SESSION AUTHORIZATION role_e;
GRANT pg_read_all_data TO role_f; -- success (cluster-wide admin option through role_a membership)
GRANT pg_read_all_data TO role_f IN DATABASE db_2; -- success (cluster-wide admin option through role_a membership)

\connect db_4
SET SESSION AUTHORIZATION role_g;
GRANT pg_read_all_data TO role_f; -- error (no cluster-wide admin option)
REVOKE pg_read_all_data FROM role_f IN CURRENT DATABASE; -- success (database-specific admin option was inherited from role_b)
GRANT pg_read_all_data TO role_f IN CURRENT DATABASE; -- success (database-specific admin option was inherited from role_b)

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
DROP DATABASE db_3;
SELECT * FROM check_memberships();
