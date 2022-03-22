-- SET commands fire both the ProcessUtility_hook and the
-- object_access_hook_str.  Since the auditing GUC starts out false, we miss the
-- initial "attempting" audit message from the ProcessUtility_hook, but we
-- should thereafter see the audit messages
LOAD 'test_oat_hooks';
SET test_oat_hooks.audit = true;

-- Create objects for use in the test
CREATE USER regress_test_user;
CREATE TABLE regress_test_table (t text);
GRANT SELECT ON Table regress_test_table TO public;
CREATE FUNCTION regress_test_func (t text) RETURNS text AS $$
	SELECT $1;
$$ LANGUAGE sql;
GRANT EXECUTE ON FUNCTION regress_test_func (text) TO public;

-- Do a few things as superuser
SELECT * FROM regress_test_table;
SELECT regress_test_func('arg');
SET work_mem = 8192;
RESET work_mem;
ALTER SYSTEM SET work_mem = 8192;
ALTER SYSTEM RESET work_mem;

-- Do those same things as non-superuser
SET SESSION AUTHORIZATION regress_test_user;
SELECT * FROM regress_test_table;
SELECT regress_test_func('arg');
SET work_mem = 8192;
RESET work_mem;
ALTER SYSTEM SET work_mem = 8192;
ALTER SYSTEM RESET work_mem;
RESET SESSION AUTHORIZATION;

-- Turn off non-superuser permissions
SET test_oat_hooks.deny_set_variable = true;
SET test_oat_hooks.deny_alter_system = true;
SET test_oat_hooks.deny_object_access = true;
SET test_oat_hooks.deny_exec_perms = true;
SET test_oat_hooks.deny_utility_commands = true;

-- Try again as non-superuser with permissions denied
SET SESSION AUTHORIZATION regress_test_user;
SELECT * FROM regress_test_table;
SELECT regress_test_func('arg');
SET work_mem = 8192;
RESET work_mem;
ALTER SYSTEM SET work_mem = 8192;
ALTER SYSTEM RESET work_mem;

RESET SESSION AUTHORIZATION;

SET test_oat_hooks.audit = false;
