CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('mk-file','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_add_key_provider_file('free-file','/tmp/pg_tde_test_keyring_2.per');
SELECT pg_tde_add_key_provider_vault_v2('V2-vault','vault-token','percona.com/vault-v2/percona','/mount/dev','ca-cert-auth');

SELECT pg_tde_set_master_key('test-db-master-key','mk-file');

-- Try dropping the in-use key provider
DELETE FROM percona_tde.pg_tde_key_provider WHERE provider_name = 'mk-file'; -- Should fail
-- Now delete the un-used  key provider
DELETE FROM percona_tde.pg_tde_key_provider WHERE provider_name = 'free-file'; -- Should pass
DELETE FROM percona_tde.pg_tde_key_provider WHERE provider_name = 'V2-vault'; -- Should pass

DROP EXTENSION pg_tde;
