CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT  * FROM pg_tde_key_info();

SELECT pg_tde_add_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_database_key_providers();
SELECT pg_tde_delete_database_key_provider('file-provider');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_add_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_database_key_providers();
SELECT pg_tde_delete_database_key_provider('file-provider');
SELECT * FROM pg_tde_list_all_database_key_providers();

SELECT pg_tde_add_database_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');
SELECT * FROM pg_tde_list_all_database_key_providers();
SELECT pg_tde_delete_database_key_provider('file-provider');
SELECT * FROM pg_tde_list_all_database_key_providers();

DROP EXTENSION pg_tde;
