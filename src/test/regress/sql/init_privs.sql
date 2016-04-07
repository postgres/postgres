-- Test iniital privileges

-- There should always be some initial privileges, set up by initdb
SELECT count(*) > 0 FROM pg_init_privs;

CREATE ROLE init_privs_test_role1;
CREATE ROLE init_privs_test_role2;

-- Intentionally include some non-initial privs for pg_dump to dump out
GRANT SELECT ON pg_proc TO init_privs_test_role1;
GRANT SELECT (prosrc) ON pg_proc TO init_privs_test_role2;
