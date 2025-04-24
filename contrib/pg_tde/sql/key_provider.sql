CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT  * FROM pg_tde_key_info();

SELECT pg_tde_add_database_key_provider_file('incorrect-file-provider',  json_object('foo' VALUE '/tmp/pg_tde_test_keyring.per'));
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_add_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_add_database_key_provider_file('file-provider2','/tmp/pg_tde_test_keyring2.per');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_verify_key();

SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-provider');
SELECT pg_tde_verify_key();

SELECT pg_tde_change_database_key_provider_file('not-existent-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_change_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring_other.per');
SELECT * FROM pg_tde_list_all_database_key_providers();
SELECT pg_tde_verify_key();

SELECT pg_tde_change_database_key_provider_file('file-provider',  json_object('foo' VALUE '/tmp/pg_tde_test_keyring.per'));
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_add_global_key_provider_file('file-keyring','/tmp/pg_tde_test_keyring.per');

SELECT pg_tde_add_global_key_provider_file('file-keyring2','/tmp/pg_tde_test_keyring2.per');

SELECT id, provider_name FROM pg_tde_list_all_global_key_providers();

-- fails
SELECT pg_tde_delete_database_key_provider('file-provider');
SELECT id, provider_name FROM pg_tde_list_all_database_key_providers();

-- works
SELECT pg_tde_delete_database_key_provider('file-provider2');
SELECT id, provider_name FROM pg_tde_list_all_database_key_providers();

SELECT id, provider_name FROM pg_tde_list_all_global_key_providers();

SELECT pg_tde_set_key_using_global_key_provider('test-db-key', 'file-keyring', false);

-- fails
SELECT pg_tde_delete_global_key_provider('file-keyring');
SELECT id, provider_name FROM pg_tde_list_all_global_key_providers();

-- works
SELECT pg_tde_delete_global_key_provider('file-keyring2');
SELECT id, provider_name FROM pg_tde_list_all_global_key_providers();

-- Creating a file key provider fails if we can't open or create the file
SELECT pg_tde_add_database_key_provider_file('will-not-work','/cant-create-file-in-root.per');

-- Creating key providers fails if any required parameter is NULL
SELECT pg_tde_add_database_key_provider(NULL, 'name', '{}');
SELECT pg_tde_add_database_key_provider('file', NULL, '{}');
SELECT pg_tde_add_database_key_provider('file', 'name', NULL);
SELECT pg_tde_add_global_key_provider(NULL, 'name', '{}');
SELECT pg_tde_add_global_key_provider('file', NULL, '{}');
SELECT pg_tde_add_global_key_provider('file', 'name', NULL);

-- Modifying key providers fails if any required parameter is NULL
SELECT pg_tde_change_database_key_provider(NULL, 'file-keyring', '{}');
SELECT pg_tde_change_database_key_provider('file', NULL, '{}');
SELECT pg_tde_change_database_key_provider('file', 'file-keyring', NULL);
SELECT pg_tde_change_global_key_provider(NULL, 'file-keyring', '{}');
SELECT pg_tde_change_global_key_provider('file', NULL, '{}');
SELECT pg_tde_change_global_key_provider('file', 'file-keyring', NULL);

-- Deleting key providers fails if key name is NULL
SELECT pg_tde_delete_database_key_provider(NULL);
SELECT pg_tde_delete_global_key_provider(NULL);

-- Setting principal key fails if key name is NULL
SELECT pg_tde_set_default_key_using_global_key_provider(NULL, 'file-keyring');
SELECT pg_tde_set_key_using_database_key_provider(NULL, 'file-keyring');
SELECT pg_tde_set_key_using_global_key_provider(NULL, 'file-keyring');
SELECT pg_tde_set_server_key_using_global_key_provider(NULL, 'file-keyring');

-- Empty string is not allowed for a principal key name
SELECT pg_tde_set_default_key_using_global_key_provider('', 'file-keyring');
SELECT pg_tde_set_key_using_database_key_provider('', 'file-keyring');
SELECT pg_tde_set_key_using_global_key_provider('', 'file-keyring');
SELECT pg_tde_set_server_key_using_global_key_provider('', 'file-keyring');

DROP EXTENSION pg_tde;
