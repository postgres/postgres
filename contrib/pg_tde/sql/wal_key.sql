-- basic tests for pg_tde_create_wal_key
-- doesn't test actual wal encryption, as that requires a server restart,
-- only sanity checks for the key creation
CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT pg_tde_create_wal_key();

SELECT pg_tde_add_global_key_provider_file('file-keyring','/tmp/pg_tde_test_keyring.per');

SELECT pg_tde_create_wal_key();

-- db local principal key with global provider
SELECT pg_tde_set_global_principal_key('test-db-principal-key', 'file-keyring', true);

SELECT pg_tde_create_wal_key();

SELECT pg_tde_set_server_principal_key('test-db-principal-key', 'file-keyring');

-- and now it should work!
SELECT pg_tde_create_wal_key();

-- and now it shouldn't create a new one!
SELECT pg_tde_create_wal_key();

DROP EXTENSION pg_tde;