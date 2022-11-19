--
-- Tests for privileges on GUCs.
-- This is unsafe because changes will affect other databases in the cluster.
--

-- Test with a superuser role.
CREATE ROLE regress_admin SUPERUSER;

-- Perform operations as user 'regress_admin'.
SET SESSION AUTHORIZATION regress_admin;

-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- ok
ALTER SYSTEM RESET ignore_system_indexes;  -- ok
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- ok
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- ok
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- ok
ALTER SYSTEM RESET autovacuum;  -- ok
-- PGC_SUSET
SET lc_messages = 'C';  -- ok
RESET lc_messages;  -- ok
ALTER SYSTEM SET lc_messages = 'C';  -- ok
ALTER SYSTEM RESET lc_messages;  -- ok
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- ok
ALTER SYSTEM RESET jit_debugging_support;  -- ok
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- ok
ALTER SYSTEM RESET DateStyle;  -- ok
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing superuser

-- Create non-superuser with privileges to configure host resource usage
CREATE ROLE regress_host_resource_admin NOSUPERUSER;
-- Revoke privileges not yet granted
REVOKE SET, ALTER SYSTEM ON PARAMETER work_mem FROM regress_host_resource_admin;
REVOKE SET, ALTER SYSTEM ON PARAMETER zero_damaged_pages FROM regress_host_resource_admin;
-- Check the new role does not yet have privileges on parameters
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET, ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'ALTER SYSTEM');
-- Check inappropriate and nonsense privilege types
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SELECT, UPDATE, CREATE');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'USAGE');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'WHATEVER');
-- Revoke, grant, and revoke again a SUSET parameter not yet granted
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'ALTER SYSTEM');
REVOKE SET ON PARAMETER zero_damaged_pages FROM regress_host_resource_admin;
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'ALTER SYSTEM');
GRANT SET ON PARAMETER zero_damaged_pages TO regress_host_resource_admin;
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'ALTER SYSTEM');
REVOKE SET ON PARAMETER zero_damaged_pages FROM regress_host_resource_admin;
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'zero_damaged_pages', 'ALTER SYSTEM');
-- Revoke, grant, and revoke again a USERSET parameter not yet granted
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'ALTER SYSTEM');
REVOKE SET ON PARAMETER work_mem FROM regress_host_resource_admin;
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'ALTER SYSTEM');
GRANT SET ON PARAMETER work_mem TO regress_host_resource_admin;
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'ALTER SYSTEM');
REVOKE SET ON PARAMETER work_mem FROM regress_host_resource_admin;
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'ALTER SYSTEM');

-- Revoke privileges from a non-existent custom GUC.  This should not create
-- entries in the catalog.
REVOKE ALL ON PARAMETER "none.such" FROM regress_host_resource_admin;
SELECT 1 FROM pg_parameter_acl WHERE parname = 'none.such';
-- Grant and then revoke privileges on the non-existent custom GUC.  Check that
-- a do-nothing entry is not left in the catalogs after the revoke.
GRANT ALL ON PARAMETER none.such TO regress_host_resource_admin;
SELECT 1 FROM pg_parameter_acl WHERE parname = 'none.such';
REVOKE ALL ON PARAMETER "None.Such" FROM regress_host_resource_admin;
SELECT 1 FROM pg_parameter_acl WHERE parname = 'none.such';
-- Can't grant on a non-existent core GUC.
GRANT ALL ON PARAMETER no_such_guc TO regress_host_resource_admin;  -- fail

-- Initially there are no privileges and no catalog entry for this GUC.
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET, ALTER SYSTEM');
SELECT 1 FROM pg_parameter_acl WHERE parname = 'enable_material';
-- GRANT SET creates an entry:
GRANT SET ON PARAMETER enable_material TO PUBLIC;
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET, ALTER SYSTEM');
SELECT 1 FROM pg_parameter_acl WHERE parname = 'enable_material';
-- Now grant ALTER SYSTEM:
GRANT ALL ON PARAMETER enable_material TO PUBLIC;
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET, ALTER SYSTEM');
SELECT 1 FROM pg_parameter_acl WHERE parname = 'enable_material';
-- REVOKE ALTER SYSTEM brings us back to just the SET privilege:
REVOKE ALTER SYSTEM ON PARAMETER enable_material FROM PUBLIC;
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'enable_material', 'SET, ALTER SYSTEM');
SELECT 1 FROM pg_parameter_acl WHERE parname = 'enable_material';
-- And this should remove the entry altogether:
REVOKE SET ON PARAMETER enable_material FROM PUBLIC;
SELECT 1 FROM pg_parameter_acl WHERE parname = 'enable_material';

-- Grant privileges on parameters to the new non-superuser role
GRANT SET, ALTER SYSTEM ON PARAMETER
    autovacuum_work_mem, hash_mem_multiplier, max_stack_depth,
    shared_buffers, temp_file_limit, work_mem
TO regress_host_resource_admin;
-- Check the new role now has privilges on parameters
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET, ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'ALTER SYSTEM');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SET WITH GRANT OPTION, ALTER SYSTEM WITH GRANT OPTION');
-- Check again the inappropriate and nonsense privilege types.  The prior
-- similar check was performed before any entry for work_mem existed.
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'SELECT, UPDATE, CREATE');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'USAGE');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'WHATEVER');
SELECT has_parameter_privilege('regress_host_resource_admin', 'work_mem', 'WHATEVER WITH GRANT OPTION');

-- Check other function signatures
SELECT has_parameter_privilege((SELECT oid FROM pg_catalog.pg_authid WHERE rolname = 'regress_host_resource_admin'),
                              'max_stack_depth',
                              'SET');
SELECT has_parameter_privilege('hash_mem_multiplier', 'set');

-- Check object identity functions
SELECT pg_describe_object(tableoid, oid, 0)
FROM pg_parameter_acl WHERE parname = 'work_mem';
SELECT pg_identify_object(tableoid, oid, 0)
FROM pg_parameter_acl WHERE parname = 'work_mem';
SELECT pg_identify_object_as_address(tableoid, oid, 0)
FROM pg_parameter_acl WHERE parname = 'work_mem';
SELECT classid::regclass,
       (SELECT parname FROM pg_parameter_acl WHERE oid = goa.objid) AS parname,
       objsubid
FROM pg_get_object_address('parameter ACL', '{work_mem}', '{}') goa;

-- Make a per-role setting that regress_host_resource_admin can't change
ALTER ROLE regress_host_resource_admin SET lc_messages = 'C';

-- Perform some operations as user 'regress_host_resource_admin'
SET SESSION AUTHORIZATION regress_host_resource_admin;
ALTER SYSTEM SET autovacuum_work_mem = 32;  -- ok, privileges have been granted
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, insufficient privileges
ALTER SYSTEM RESET autovacuum_multixact_freeze_max_age;  -- fail, insufficient privileges
SET jit_provider = 'llvmjit';  -- fail, insufficient privileges
SELECT set_config ('jit_provider', 'llvmjit', true); -- fail, insufficient privileges
ALTER SYSTEM SET shared_buffers = 50;  -- ok
ALTER SYSTEM RESET shared_buffers;  -- ok
SET autovacuum_work_mem = 50;  -- cannot be changed now
ALTER SYSTEM RESET temp_file_limit;  -- ok
SET TimeZone = 'Europe/Helsinki';  -- ok
RESET TimeZone;  -- ok
SET max_stack_depth = '100kB';  -- ok, privileges have been granted
RESET max_stack_depth;  -- ok, privileges have been granted
ALTER SYSTEM SET max_stack_depth = '100kB';  -- ok, privileges have been granted
ALTER SYSTEM RESET max_stack_depth;  -- ok, privileges have been granted
SET lc_messages = 'C';  -- fail, insufficient privileges
RESET lc_messages;  -- fail, insufficient privileges
ALTER SYSTEM SET lc_messages = 'C';  -- fail, insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, insufficient privileges
SELECT set_config ('temp_buffers', '8192', false); -- ok
ALTER SYSTEM RESET autovacuum_work_mem;  -- ok, privileges have been granted
ALTER SYSTEM RESET ALL;  -- fail, insufficient privileges
ALTER ROLE regress_host_resource_admin SET lc_messages = 'POSIX';  -- fail
ALTER ROLE regress_host_resource_admin SET max_stack_depth = '1MB';  -- ok
SELECT setconfig FROM pg_db_role_setting
  WHERE setrole = 'regress_host_resource_admin'::regrole;
ALTER ROLE regress_host_resource_admin RESET max_stack_depth;  -- ok
SELECT setconfig FROM pg_db_role_setting
  WHERE setrole = 'regress_host_resource_admin'::regrole;
ALTER ROLE regress_host_resource_admin SET max_stack_depth = '1MB';  -- ok
SELECT setconfig FROM pg_db_role_setting
  WHERE setrole = 'regress_host_resource_admin'::regrole;
ALTER ROLE regress_host_resource_admin RESET ALL;  -- doesn't reset lc_messages
SELECT setconfig FROM pg_db_role_setting
  WHERE setrole = 'regress_host_resource_admin'::regrole;

-- Check dropping/revoking behavior
SET SESSION AUTHORIZATION regress_admin;
DROP ROLE regress_host_resource_admin;  -- fail, privileges remain
-- Use "revoke" to remove the privileges and allow the role to be dropped
REVOKE SET, ALTER SYSTEM ON PARAMETER
    autovacuum_work_mem, hash_mem_multiplier, max_stack_depth,
    shared_buffers, temp_file_limit, work_mem
FROM regress_host_resource_admin;
DROP ROLE regress_host_resource_admin;  -- ok

-- Try that again, but use "drop owned by" instead of "revoke"
CREATE ROLE regress_host_resource_admin NOSUPERUSER;
SET SESSION AUTHORIZATION regress_host_resource_admin;
ALTER SYSTEM SET autovacuum_work_mem = 32;  -- fail, privileges not yet granted
SET SESSION AUTHORIZATION regress_admin;
GRANT SET, ALTER SYSTEM ON PARAMETER
    autovacuum_work_mem, hash_mem_multiplier, max_stack_depth,
    shared_buffers, temp_file_limit, work_mem
TO regress_host_resource_admin;
DROP ROLE regress_host_resource_admin;  -- fail, privileges remain
DROP OWNED BY regress_host_resource_admin RESTRICT; -- cascade should not be needed
SET SESSION AUTHORIZATION regress_host_resource_admin;
ALTER SYSTEM SET autovacuum_work_mem = 32;  -- fail, "drop owned" has dropped privileges
SET SESSION AUTHORIZATION regress_admin;
DROP ROLE regress_host_resource_admin;  -- ok

-- Check that "reassign owned" doesn't affect privileges
CREATE ROLE regress_host_resource_admin NOSUPERUSER;
CREATE ROLE regress_host_resource_newadmin NOSUPERUSER;
GRANT SET, ALTER SYSTEM ON PARAMETER
    autovacuum_work_mem, hash_mem_multiplier, max_stack_depth,
    shared_buffers, temp_file_limit, work_mem
TO regress_host_resource_admin;
REASSIGN OWNED BY regress_host_resource_admin TO regress_host_resource_newadmin;
SET SESSION AUTHORIZATION regress_host_resource_admin;
ALTER SYSTEM SET autovacuum_work_mem = 32;  -- ok, "reassign owned" did not change privileges
ALTER SYSTEM RESET autovacuum_work_mem;  -- ok
SET SESSION AUTHORIZATION regress_admin;
DROP ROLE regress_host_resource_admin;  -- fail, privileges remain
DROP ROLE regress_host_resource_newadmin;  -- ok, nothing was transferred
-- Use "drop owned by" so we can drop the role
DROP OWNED BY regress_host_resource_admin;  -- ok
DROP ROLE regress_host_resource_admin;  -- ok

-- Clean up
RESET SESSION AUTHORIZATION;
DROP ROLE regress_admin; -- ok
