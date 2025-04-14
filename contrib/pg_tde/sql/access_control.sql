CREATE EXTENSION IF NOT EXISTS pg_tde;

CREATE USER regress_pg_tde_access_control;

SET ROLE regress_pg_tde_access_control;

-- should throw access denied
SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');

RESET ROLE;

SELECT pg_tde_grant_database_key_management_to_role('regress_pg_tde_access_control');
SELECT pg_tde_grant_key_viewer_to_role('regress_pg_tde_access_control');

SET ROLE regress_pg_tde_access_control;

-- should now be allowed
SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_add_database_key_provider_file('file-2', '/tmp/pg_tde_test_keyring_2.per');
SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');
SELECT * FROM pg_tde_list_all_database_key_providers();
SELECT key_name, key_provider_name, key_provider_id FROM pg_tde_key_info();

-- only superuser
SELECT pg_tde_add_global_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_change_global_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_delete_global_key_provider('file-vault');
SELECT pg_tde_set_key_using_global_key_provider('key1', 'file-vault');
SELECT pg_tde_set_default_key_using_global_key_provider('key1', 'file-vault');
SELECT pg_tde_set_server_key_using_global_key_provider('key1', 'file-vault');

RESET ROLE;

SELECT pg_tde_revoke_key_viewer_from_role('regress_pg_tde_access_control');

SET ROLE regress_pg_tde_access_control;

-- verify the view access is revoked
SELECT * FROM pg_tde_list_all_database_key_providers();
SELECT key_name, key_provider_name, key_provider_id FROM pg_tde_key_info();

RESET ROLE;

DROP EXTENSION pg_tde CASCADE;
