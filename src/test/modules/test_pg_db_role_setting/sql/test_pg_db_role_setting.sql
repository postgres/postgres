CREATE EXTENSION test_pg_db_role_setting;
CREATE USER regress_super_user SUPERUSER;
CREATE USER regress_regular_user;

\c - regress_regular_user
-- successfully set a placeholder value
SET test_pg_db_role_setting.superuser_param = 'aaa';

-- module is loaded, the placeholder value is thrown away
SELECT load_test_pg_db_role_setting();

SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

\c - regress_regular_user
-- fail, not privileges
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.superuser_param = 'aaa';
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.user_param = 'bbb';
-- success for USER SET parameters
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.superuser_param = 'aaa' USER SET;
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.user_param = 'bbb' USER SET;

\drds regress_regular_user

\c - regress_regular_user
-- successfully set placeholders
SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

-- module is loaded, the placeholder value of superuser param is thrown away
SELECT load_test_pg_db_role_setting();

SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

\c - regress_super_user
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.superuser_param = 'aaa';
\drds regress_regular_user

\c - regress_regular_user
-- don't have a priviledge to change superuser value to user set one
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.superuser_param = 'ccc' USER SET;

\c - regress_super_user
SELECT load_test_pg_db_role_setting();
-- give the privilege to set SUSET param to the regular user
GRANT SET ON PARAMETER test_pg_db_role_setting.superuser_param TO regress_regular_user;

\c - regress_regular_user
ALTER ROLE regress_regular_user SET test_pg_db_role_setting.superuser_param = 'ccc';

\drds regress_regular_user

\c - regress_regular_user
-- successfully set placeholders
SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

-- module is loaded, and placeholder values are successfully set
SELECT load_test_pg_db_role_setting();

SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;
