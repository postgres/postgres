CREATE SCHEMA tde;
CREATE EXTENSION IF NOT EXISTS pg_tde SCHEMA tde;

SELECT tde.pg_tde_add_global_key_provider_file('reg_file-global', '/tmp/pg_tde_test_keyring.per');
SELECT tde.pg_tde_set_server_principal_key('global-principal-key', 'reg_file-global');
ALTER SYSTEM SET pg_tde.wal_encrypt = on;
ALTER SYSTEM SET default_table_access_method = 'tde_heap';
ALTER SYSTEM SET search_path = "$user",public,tde;
-- restart required
