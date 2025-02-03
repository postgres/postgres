CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT  * FROM pg_tde_principal_key_info();

SELECT pg_tde_add_key_provider_file('incorrect-file-provider',  json_object('foo' VALUE '/tmp/pg_tde_test_keyring.per'));
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_add_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_add_key_provider_file('file-provider2','/tmp/pg_tde_test_keyring2.per');
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_set_principal_key('test-db-principal-key','file-provider');

SELECT pg_tde_change_key_provider_file('not-existent-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_change_key_provider_file('file-provider','/tmp/pg_tde_test_keyring_other.per');
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_change_key_provider_file('file-provider',  json_object('foo' VALUE '/tmp/pg_tde_test_keyring.per'));
SELECT * FROM pg_tde_list_all_key_providers();

SELECT pg_tde_add_key_provider_file('PG_TDE_GLOBAL', 'file-keyring','/tmp/pg_tde_test_keyring.per');

SELECT pg_tde_add_key_provider_file('PG_TDE_GLOBAL', 'file-keyring2','/tmp/pg_tde_test_keyring2.per');

SELECT id, provider_name FROM pg_tde_list_all_key_providers('PG_TDE_GLOBAL');

-- TODO: verify that we can also can change the type of it

-- fails
SELECT pg_tde_delete_key_provider('file-provider');
SELECT id, provider_name FROM pg_tde_list_all_key_providers();

-- works
SELECT pg_tde_delete_key_provider('file-provider2');
SELECT id, provider_name FROM pg_tde_list_all_key_providers();

SELECT id, provider_name FROM pg_tde_list_all_key_providers('PG_TDE_GLOBAL');

SELECT pg_tde_set_principal_key('test-db-principal-key', 'PG_TDE_GLOBAL', 'file-keyring', false);

-- fails
SELECT pg_tde_delete_key_provider('PG_TDE_GLOBAL', 'file-keyring');
SELECT id, provider_name FROM pg_tde_list_all_key_providers('PG_TDE_GLOBAL');

-- works
SELECT pg_tde_delete_key_provider('PG_TDE_GLOBAL', 'file-keyring2');
SELECT id, provider_name FROM pg_tde_list_all_key_providers('PG_TDE_GLOBAL');

DROP EXTENSION pg_tde;