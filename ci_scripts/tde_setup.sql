CREATE EXTENSION IF NOT EXISTS pg_tde;
SELECT pg_tde_add_key_provider_file('reg_file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key', 'reg_file-vault');
ALTER SYSTEM SET default_table_access_method='tde_heap';
SET default_table_access_method='tde_heap';
SELECT pg_reload_conf();