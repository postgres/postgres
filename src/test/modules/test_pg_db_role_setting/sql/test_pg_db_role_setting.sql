CREATE EXTENSION test_pg_db_role_setting;
CREATE USER super_user SUPERUSER;
CREATE USER regular_user;

\c - regular_user
-- successfully set a placeholder value
SET test_pg_db_role_setting.superuser_param = 'aaa';

-- module is loaded, the placeholder value is thrown away
SELECT load_test_pg_db_role_setting();

SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

\c - regular_user
-- fail, not privileges
ALTER ROLE regular_user SET test_pg_db_role_setting.superuser_param = 'aaa';
ALTER ROLE regular_user SET test_pg_db_role_setting.user_param = 'bbb';
-- success for USER SET parameters
ALTER ROLE regular_user SET test_pg_db_role_setting.superuser_param = 'aaa' USER SET;
ALTER ROLE regular_user SET test_pg_db_role_setting.user_param = 'bbb' USER SET;

\drds regular_user

\c - regular_user
-- successfully set placeholders
SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

-- module is loaded, the placeholder value of superuser param is thrown away
SELECT load_test_pg_db_role_setting();

SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

\c - super_user
ALTER ROLE regular_user SET test_pg_db_role_setting.superuser_param = 'aaa';
\drds regular_user

\c - regular_user
-- don't have a priviledge to change superuser value to user set one
ALTER ROLE regular_user SET test_pg_db_role_setting.superuser_param = 'ccc' USER SET;

\c - super_user
SELECT load_test_pg_db_role_setting();
-- give the privilege to set SUSET param to the regular user
GRANT SET ON PARAMETER test_pg_db_role_setting.superuser_param TO regular_user;

\c - regular_user
ALTER ROLE regular_user SET test_pg_db_role_setting.superuser_param = 'ccc';

\drds regular_user

\c - regular_user
-- successfully set placeholders
SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;

-- module is loaded, and placeholder values are succesfully set
SELECT load_test_pg_db_role_setting();

SHOW test_pg_db_role_setting.superuser_param;
SHOW test_pg_db_role_setting.user_param;
