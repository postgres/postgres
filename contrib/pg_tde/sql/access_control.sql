CREATE EXTENSION IF NOT EXISTS pg_tde;

CREATE USER regress_pg_tde_access_control;

SET ROLE regress_pg_tde_access_control;

-- should throw access denied
SELECT pg_tde_add_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key', 'file-vault');

RESET ROLE;

SELECT pg_tde_grant_local_key_management_to_role('regress_pg_tde_access_control');
SELECT pg_tde_grant_key_viewer_to_role('regress_pg_tde_access_control');

SET ROLE regress_pg_tde_access_control;

-- should now be allowed
SELECT pg_tde_add_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_add_key_provider_file('file-2', '/tmp/pg_tde_test_keyring_2.per');
SELECT pg_tde_set_principal_key('test-db-principal-key', 'file-vault');
SELECT * FROM pg_tde_list_all_key_providers();
SELECT principal_key_name, key_provider_name, key_provider_id FROM pg_tde_principal_key_info();

RESET ROLE;

SELECT pg_tde_revoke_key_viewer_from_role('regress_pg_tde_access_control');

SET ROLE regress_pg_tde_access_control;

-- verify the view access is revoked
SELECT * FROM pg_tde_list_all_key_providers();
SELECT principal_key_name, key_provider_name, key_provider_id FROM pg_tde_principal_key_info();

RESET ROLE;

DROP EXTENSION pg_tde CASCADE;
