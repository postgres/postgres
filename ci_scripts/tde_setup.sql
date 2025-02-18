CREATE SCHEMA IF NOT EXISTS tde;
CREATE EXTENSION IF NOT EXISTS pg_tde SCHEMA tde;
SELECT pg_tde_add_key_provider_file('reg_file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key', 'reg_file-vault');
