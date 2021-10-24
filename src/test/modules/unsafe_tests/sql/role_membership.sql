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
  m.rolname LIKE 'role_%'
ORDER BY
  1, 2, 5
$$ LANGUAGE SQL;

-- Populate test databases
\connect template1
CREATE TABLE data AS SELECT generate_series(1, 3);

CREATE DATABASE db_1;
CREATE DATABASE db_2;
CREATE DATABASE db_3;
CREATE DATABASE db_4;

-- Read all cluster-wide with admin option
CREATE ROLE role_read_all_with_admin;
GRANT pg_read_all_data TO role_read_all_with_admin WITH ADMIN OPTION;

-- Read all in databases 1 and 2
CREATE ROLE role_read_12;
GRANT pg_read_all_data TO role_read_12 IN DATABASE db_1;
GRANT pg_read_all_data TO role_read_12 IN DATABASE db_2;

-- Read all in databases 3 and 4 with admin option
CREATE ROLE role_read_34;
GRANT pg_read_all_data TO role_read_34 IN DATABASE db_3 WITH ADMIN OPTION;
GRANT pg_read_all_data TO role_read_34 IN DATABASE db_4 WITH ADMIN OPTION;

-- Inherits read all in databases 3 and 4
CREATE ROLE role_inherited_34;
GRANT role_read_34 TO role_inherited_34;

-- Inherits read all in database 3
CREATE ROLE role_inherited_3;
GRANT role_read_34 TO role_inherited_3 IN DATABASE db_3;

-- No inherit
CREATE ROLE role_read_all_noinherit NOINHERIT;
GRANT role_read_all_with_admin TO role_read_all_noinherit;

-- No inherit in databases 1 and 2
CREATE ROLE role_read_12_noinherit NOINHERIT;
GRANT role_read_12 TO role_read_12_noinherit;

-- Alternate syntax
CREATE ROLE role_read_template1;
GRANT pg_read_all_data TO role_read_template1, role_read_all_noinherit IN CURRENT DATABASE;

-- Failure due to missing database
GRANT pg_read_all_data TO role_read_template1 IN DATABASE non_existent; -- error

-- Should warn on duplicate grants
GRANT pg_read_all_data TO role_read_all_with_admin; -- notice
GRANT pg_read_all_data TO role_read_template1 IN DATABASE template1; -- notice

-- Should not warn if adjusting admin option
GRANT pg_read_all_data TO role_read_template1 IN DATABASE template1 WITH ADMIN OPTION; -- silent
GRANT pg_read_all_data TO role_read_template1 IN DATABASE template1 WITH ADMIN OPTION; -- notice

-- Check membership table
\connect postgres role_admin
SELECT * FROM check_memberships();

-- Test membership privileges (db_1)
\connect db_1
SET ROLE role_read_all_with_admin;
SELECT * FROM data; -- success
SET ROLE role_read_12;
SELECT * FROM data; -- success
SET ROLE role_read_34;
SELECT * FROM data; -- error
SET ROLE role_inherited_34;
SELECT * FROM data; -- error
SET ROLE role_inherited_3;
SELECT * FROM data; -- error
SET ROLE role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12_noinherit;
SELECT * FROM data; -- error

SET SESSION AUTHORIZATION role_read_12;
SET ROLE pg_read_all_data; -- success

SET SESSION AUTHORIZATION role_inherited_34;
SET ROLE pg_read_all_data; -- error
SET ROLE role_read_34; -- success

SET SESSION AUTHORIZATION role_inherited_3;
SET ROLE pg_read_all_data; -- error
SET ROLE role_read_34; -- error

SET SESSION AUTHORIZATION role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE pg_read_all_data; -- success
SELECT * FROM data; -- success

SET SESSION AUTHORIZATION role_read_12_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12; -- success
SELECT * FROM data; -- success

-- Test membership privileges (db_2)
\connect db_2
SET ROLE role_read_all_with_admin;
SELECT * FROM data; -- success
SET ROLE role_read_12;
SELECT * FROM data; -- success
SET ROLE role_read_34;
SELECT * FROM data; -- error
SET ROLE role_inherited_34;
SELECT * FROM data; -- error
SET ROLE role_inherited_3;
SELECT * FROM data; -- error
SET ROLE role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12_noinherit;
SELECT * FROM data; -- error

SET SESSION AUTHORIZATION role_read_12;
SET ROLE pg_read_all_data; -- success

SET SESSION AUTHORIZATION role_inherited_34;
SET ROLE pg_read_all_data; -- error
SET ROLE role_read_34; -- success

SET SESSION AUTHORIZATION role_inherited_3;
SET ROLE pg_read_all_data; -- error
SET ROLE role_read_34; -- error

SET SESSION AUTHORIZATION role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE pg_read_all_data; -- success
SELECT * FROM data; -- success

SET SESSION AUTHORIZATION role_read_12_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12; -- success
SELECT * FROM data; -- success

-- Test membership privileges (db_3)
\connect db_3
SET ROLE role_read_all_with_admin;
SELECT * FROM data; -- success
SET ROLE role_read_12;
SELECT * FROM data; -- error
SET ROLE role_read_34;
SELECT * FROM data; -- success
SET ROLE role_inherited_34;
SELECT * FROM data; -- success
SET ROLE role_inherited_3;
SELECT * FROM data; -- success
SET ROLE role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12_noinherit;
SELECT * FROM data; -- error

SET SESSION AUTHORIZATION role_read_12;
SET ROLE pg_read_all_data; -- error

SET SESSION AUTHORIZATION role_inherited_34;
SET ROLE pg_read_all_data; -- success
SET ROLE role_read_34; -- success

SET SESSION AUTHORIZATION role_inherited_3;
SET ROLE pg_read_all_data; -- success
SET ROLE role_read_34; -- success

SET SESSION AUTHORIZATION role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE pg_read_all_data; -- success
SELECT * FROM data; -- success

SET SESSION AUTHORIZATION role_read_12_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12; -- error
SELECT * FROM data; -- error

-- Test membership privileges (db_4)
\connect db_4
SET ROLE role_read_all_with_admin;
SELECT * FROM data; -- success
SET ROLE role_read_12;
SELECT * FROM data; -- error
SET ROLE role_read_34;
SELECT * FROM data; -- success
SET ROLE role_inherited_34;
SELECT * FROM data; -- success
SET ROLE role_inherited_3;
SELECT * FROM data; -- error
SET ROLE role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12_noinherit;
SELECT * FROM data; -- error

SET SESSION AUTHORIZATION role_read_12;
SET ROLE pg_read_all_data; -- error

SET SESSION AUTHORIZATION role_inherited_34;
SET ROLE pg_read_all_data; -- success
SET ROLE role_read_34; -- success

SET SESSION AUTHORIZATION role_inherited_3;
SET ROLE pg_read_all_data; -- error
SET ROLE role_read_34; -- error

SET SESSION AUTHORIZATION role_read_all_noinherit;
SELECT * FROM data; -- error
SET ROLE pg_read_all_data; -- success
SELECT * FROM data; -- success

SET SESSION AUTHORIZATION role_read_12_noinherit;
SELECT * FROM data; -- error
SET ROLE role_read_12; -- error
SELECT * FROM data; -- error

\connect postgres role_admin

-- Should not warn if revoking admin option
REVOKE ADMIN OPTION FOR pg_read_all_data FROM role_read_template1 IN DATABASE template1; -- silent
REVOKE ADMIN OPTION FOR pg_read_all_data FROM role_read_template1 IN DATABASE template1; -- silent
SELECT * FROM check_memberships();

-- Should warn if revoking a non-existent membership
REVOKE pg_read_all_data FROM role_read_template1 IN DATABASE template1; -- success
REVOKE pg_read_all_data FROM role_read_template1 IN DATABASE template1; -- warning
SELECT * FROM check_memberships();

-- Revoke should only apply to the specified level
REVOKE pg_read_all_data FROM role_read_12; -- warning
SELECT * FROM check_memberships();

-- Ensure cluster-wide admin option can grant cluster-wide and in specific databases
CREATE ROLE role_granted;
SET SESSION AUTHORIZATION role_read_all_with_admin;
GRANT pg_read_all_data TO role_granted; -- success
GRANT pg_read_all_data TO role_granted IN CURRENT DATABASE; -- success
GRANT pg_read_all_data TO role_granted IN DATABASE db_1; -- success
GRANT role_read_34 TO role_granted; -- error
SELECT * FROM check_memberships();

-- Ensure database-specific admin option can only grant within that database
SET SESSION AUTHORIZATION role_read_34;
GRANT pg_read_all_data TO role_granted; -- error
GRANT pg_read_all_data TO role_granted IN CURRENT DATABASE; -- error
GRANT pg_read_all_data TO role_granted IN DATABASE db_3; -- error
GRANT pg_read_all_data TO role_granted IN DATABASE db_4; -- error

\connect db_3
SET SESSION AUTHORIZATION role_read_34;
GRANT pg_read_all_data TO role_granted; -- error
GRANT pg_read_all_data TO role_granted IN CURRENT DATABASE; -- success
GRANT pg_read_all_data TO role_granted IN DATABASE db_3; -- notice
GRANT pg_read_all_data TO role_granted IN DATABASE db_4; -- error

\connect db_4
SET SESSION AUTHORIZATION role_read_34;
GRANT pg_read_all_data TO role_granted; -- error
GRANT pg_read_all_data TO role_granted IN CURRENT DATABASE; -- success
GRANT pg_read_all_data TO role_granted IN DATABASE db_3; -- error
GRANT pg_read_all_data TO role_granted IN DATABASE db_4; -- notice

\connect postgres role_admin
SELECT * FROM check_memberships();

-- Should clean up the membership table when dropping a database
\connect postgres role_admin
DROP DATABASE db_3;
SELECT * FROM check_memberships();

-- Should clean up the membership table when dropping a role
DROP ROLE role_read_34;
SELECT * FROM check_memberships();
