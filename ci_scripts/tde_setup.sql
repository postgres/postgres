CREATE SCHEMA IF NOT EXISTS tde;
CREATE EXTENSION IF NOT EXISTS pg_tde SCHEMA tde;
SELECT tde.pg_tde_add_database_key_provider_file('reg_file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT tde.pg_tde_set_key_using_database_key_provider('test-db-key', 'reg_file-vault');
