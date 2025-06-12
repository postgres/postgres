\! rm -f '/tmp/db-provider-file'
\! rm -f '/tmp/global-provider-file-1'
\! rm -f '/tmp/pg_tde_test_keyring.per'
\! rm -f '/tmp/pg_tde_test_keyring2.per'

CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT  * FROM pg_tde_key_info();

SELECT pg_tde_add_database_key_provider('file', 'incorrect-file-provider',  '{"path": {"foo": "/tmp/pg_tde_test_keyring.per"}}');
SELECT pg_tde_add_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_add_database_key_provider_file('file-provider2','/tmp/pg_tde_test_keyring2.per');
SELECT pg_tde_add_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring_dup.per');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_create_key_using_database_key_provider('test-db-key','file-provider');

SELECT pg_tde_verify_key();
SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-provider');
SELECT pg_tde_verify_key();

SELECT pg_tde_change_database_key_provider_file('not-existent-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_change_database_key_provider('file', 'file-provider', '{"path": {"foo": "/tmp/pg_tde_test_keyring.per"}}');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_add_global_key_provider_file('file-keyring','/tmp/pg_tde_test_keyring.per');

SELECT pg_tde_add_global_key_provider_file('file-keyring2','/tmp/pg_tde_test_keyring2.per');

SELECT id, name FROM pg_tde_list_all_global_key_providers();

-- fails
SELECT pg_tde_delete_database_key_provider('file-provider');
SELECT id, name FROM pg_tde_list_all_database_key_providers();

-- works
SELECT pg_tde_delete_database_key_provider('file-provider2');
SELECT id, name FROM pg_tde_list_all_database_key_providers();

SELECT id, name FROM pg_tde_list_all_global_key_providers();

SELECT pg_tde_set_key_using_global_key_provider('test-db-key', 'file-keyring');

-- fails
SELECT pg_tde_delete_global_key_provider('file-keyring');
SELECT id, name FROM pg_tde_list_all_global_key_providers();

-- works
SELECT pg_tde_delete_global_key_provider('file-keyring2');
SELECT id, name FROM pg_tde_list_all_global_key_providers();

-- Creating a file key provider fails if we can't open or create the file
SELECT pg_tde_add_database_key_provider_file('will-not-work','/cant-create-file-in-root.per');

-- Creating key providers fails if any required parameter is NULL
SELECT pg_tde_add_database_key_provider(NULL, 'name', '{}');
SELECT pg_tde_add_database_key_provider('file', NULL, '{}');
SELECT pg_tde_add_database_key_provider('file', 'name', NULL);
SELECT pg_tde_add_global_key_provider(NULL, 'name', '{}');
SELECT pg_tde_add_global_key_provider('file', NULL, '{}');
SELECT pg_tde_add_global_key_provider('file', 'name', NULL);

-- Empty string is not allowed for a key provider name
SELECT pg_tde_add_database_key_provider('file', '', '{}');
SELECT pg_tde_add_global_key_provider('file', '', '{}');

-- Creating key providers fails if the name is too long
SELECT pg_tde_add_database_key_provider('file', repeat('K', 128), '{}');
SELECT pg_tde_add_global_key_provider('file', repeat('K', 128), '{}');

-- Creating key providers fails if options is too long
SELECT pg_tde_add_database_key_provider('file', 'name', json_build_object('key', repeat('K', 1024)));
SELECT pg_tde_add_global_key_provider('file', 'name', json_build_object('key', repeat('K', 1024)));

-- Creating key providers fails if configuration is not a JSON object
SELECT pg_tde_add_database_key_provider('file', 'provider', '"bare string"');
SELECT pg_tde_add_database_key_provider('file', 'provider', '["array"]');
SELECT pg_tde_add_database_key_provider('file', 'provider', 'true');
SELECT pg_tde_add_database_key_provider('file', 'provider', 'null');

-- Creating key providers fails if vaules are not scalar
SELECT pg_tde_add_database_key_provider('file', 'provider', '{"path": {}}');
SELECT pg_tde_add_database_key_provider('file', 'provider', '{"path": ["array"]}');
SELECT pg_tde_add_database_key_provider('file', 'provider', '{"path": true}');

-- Modifying key providers fails if any required parameter is NULL
SELECT pg_tde_change_database_key_provider(NULL, 'file-keyring', '{}');
SELECT pg_tde_change_database_key_provider('file', NULL, '{}');
SELECT pg_tde_change_database_key_provider('file', 'file-keyring', NULL);
SELECT pg_tde_change_global_key_provider(NULL, 'file-keyring', '{}');
SELECT pg_tde_change_global_key_provider('file', NULL, '{}');
SELECT pg_tde_change_global_key_provider('file', 'file-keyring', NULL);

-- Modifying key providers fails if options is too long
SELECT pg_tde_change_database_key_provider('file', 'file-provider', json_build_object('key', repeat('V', 1024)));
SELECT pg_tde_change_global_key_provider('file', 'file-keyring', json_build_object('key', repeat('V', 1024)));

-- Modifying key providers fails if configuration is not a JSON object
SELECT pg_tde_change_database_key_provider('file', 'file-provider', '"bare string"');
SELECT pg_tde_change_database_key_provider('file', 'file-provider', '["array"]');
SELECT pg_tde_change_database_key_provider('file', 'file-provider', 'true');
SELECT pg_tde_change_database_key_provider('file', 'file-provider', 'null');

-- Modifying key providers fails if vaules are not scalar
SELECT pg_tde_change_database_key_provider('file', 'file-provider', '{"path": {}}');
SELECT pg_tde_change_database_key_provider('file', 'file-provider', '{"path": ["array"]}');
SELECT pg_tde_change_database_key_provider('file', 'file-provider', '{"path": true}');

-- Modifying key providers fails if new settings can't fetch existing server key
SELECT pg_tde_add_global_key_provider_file('global-provider', '/tmp/global-provider-file-1');
SELECT pg_tde_create_key_using_global_key_provider('server-key', 'global-provider');
SELECT pg_tde_set_server_key_using_global_key_provider('server-key', 'global-provider');
SELECT pg_tde_change_global_key_provider_file('global-provider','/tmp/global-provider-file-2');

-- Modifying key providers fails if new settings can't fetch existing database key
SELECT pg_tde_add_global_key_provider_file('global-provider2', '/tmp/global-provider-file-1');
SELECT current_database() AS regress_database
\gset
CREATE DATABASE db_using_global_provider;
\c db_using_global_provider;
CREATE EXTENSION pg_tde;
SELECT pg_tde_create_key_using_global_key_provider('database-key', 'global-provider2');
SELECT pg_tde_set_key_using_global_key_provider('database-key', 'global-provider2');
\c :regress_database
SELECT pg_tde_change_global_key_provider_file('global-provider2', '/tmp/global-provider-file-2');
DROP DATABASE db_using_global_provider;
CREATE DATABASE db_using_database_provider;
\c db_using_database_provider;
CREATE EXTENSION pg_tde;
SELECT pg_tde_add_database_key_provider_file('db-provider', '/tmp/db-provider-file');
SELECT pg_tde_create_key_using_database_key_provider('database-key', 'db-provider');
SELECT pg_tde_set_key_using_database_key_provider('database-key', 'db-provider');
SELECT pg_tde_change_database_key_provider_file('db-provider', '/tmp/db-provider-file-2');
\c :regress_database
DROP DATABASE db_using_database_provider;

-- Deleting key providers fails if key name is NULL
SELECT pg_tde_delete_database_key_provider(NULL);
SELECT pg_tde_delete_global_key_provider(NULL);

-- Setting principal key fails if provider name is NULL
SELECT pg_tde_set_default_key_using_global_key_provider('key', NULL);
SELECT pg_tde_set_key_using_database_key_provider('key', NULL);
SELECT pg_tde_set_key_using_global_key_provider('key', NULL);
SELECT pg_tde_set_server_key_using_global_key_provider('key', NULL);

-- Setting principal key fails if key name is NULL
SELECT pg_tde_set_default_key_using_global_key_provider(NULL, 'file-keyring');
SELECT pg_tde_set_key_using_database_key_provider(NULL, 'file-keyring');
SELECT pg_tde_set_key_using_global_key_provider(NULL, 'file-keyring');
SELECT pg_tde_set_server_key_using_global_key_provider(NULL, 'file-keyring');

-- Empty string is not allowed for a principal key name
SELECT pg_tde_create_key_using_database_key_provider('', 'file-provider');
SELECT pg_tde_create_key_using_global_key_provider('', 'file-keyring');

-- Creating principal key fails if the key name is too long
SELECT pg_tde_create_key_using_database_key_provider(repeat('K', 256), 'file-provider');
SELECT pg_tde_create_key_using_global_key_provider(repeat('K', 256), 'file-keyring');

-- Creating principal key fails if key already exists
SELECT pg_tde_create_key_using_database_key_provider('existing-key','file-provider');
SELECT pg_tde_create_key_using_database_key_provider('existing-key','file-provider');
SELECT pg_tde_create_key_using_global_key_provider('existing-key','file-keyring');

-- Setting principal key fails if key does not exist
SELECT pg_tde_set_default_key_using_global_key_provider('not-existing', 'file-keyring');
SELECT pg_tde_set_key_using_database_key_provider('not-existing', 'file-keyring');
SELECT pg_tde_set_key_using_global_key_provider('not-existing', 'file-keyring');
SELECT pg_tde_set_server_key_using_global_key_provider('not-existing', 'file-keyring');

DROP EXTENSION pg_tde;
