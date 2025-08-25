CREATE SCHEMA IF NOT EXISTS _pg_tde;
CREATE EXTENSION IF NOT EXISTS pg_tde SCHEMA _pg_tde;
\! rm -f '/tmp/pg_tde_test_keyring.per'
SELECT _pg_tde.pg_tde_add_database_key_provider_file('reg_file-vault', '/tmp/pg_tde_test_keyring.per');
SELECT _pg_tde.pg_tde_create_key_using_database_key_provider('test-db-key', 'reg_file-vault');
SELECT _pg_tde.pg_tde_set_key_using_database_key_provider('test-db-key', 'reg_file-vault');
