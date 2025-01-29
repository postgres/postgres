CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT  * FROM pg_tde_principal_key_info();

SELECT pg_tde_add_key_provider_file('incorrect-file-provider',  json_object('foo' VALUE '/tmp/pg_tde_test_keyring.per'));
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_add_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_key_providers();

--SELECT pg_tde_set_principal_key('test-db-principal-key','file-provider');

SELECT pg_tde_change_key_provider_file('not-existent-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_change_key_provider_file('file-provider','/tmp/pg_tde_test_keyring_other.per');
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_change_key_provider_file('file-provider',  json_object('foo' VALUE '/tmp/pg_tde_test_keyring.per'));
SELECT * FROM pg_tde_list_all_key_providers();

-- TODO: verify that we can also can change the type of it