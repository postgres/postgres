CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('mk-file','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_add_key_provider_file('free-file','/tmp/pg_tde_test_keyring_2.per');
SELECT pg_tde_add_key_provider_vault_v2('V2-vault','vault-token','percona.com/vault-v2/percona','/mount/dev','ca-cert-auth');

SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_set_principal_key('test-db-principal-key','mk-file');

DROP EXTENSION pg_tde;
