CREATE SCHEMA IF NOT EXISTS _pg_tde;
CREATE EXTENSION IF NOT EXISTS pg_tde SCHEMA _pg_tde;

\! rm -f '/tmp/pg_tde_test_keyring.per'
SELECT _pg_tde.pg_tde_add_global_key_provider_file('reg_file-global', '/tmp/pg_tde_test_keyring.per');
SELECT _pg_tde.pg_tde_create_key_using_global_key_provider('server-key', 'reg_file-global');
SELECT _pg_tde.pg_tde_set_server_key_using_global_key_provider('server-key', 'reg_file-global');
ALTER SYSTEM SET pg_tde.wal_encrypt = on;
ALTER SYSTEM SET default_table_access_method = 'tde_heap';
-- restart required
