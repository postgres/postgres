CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT pg_tde_add_global_key_provider_file('file-provider','/tmp/pg_tde_test_keyring.per');

-- Set the local key and delete it without any encrypted tables
-- Should succeed: nothing used the key
SELECT pg_tde_set_key_using_global_key_provider('test-db-key','file-provider');
SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();
SELECT pg_tde_delete_key();

-- Set local key, encrypt a table, and delete the key
-- Should fail: the is no default key to fallback
SELECT pg_tde_set_key_using_global_key_provider('test-db-key','file-provider');
CREATE TABLE test_table (id int, data text) USING tde_heap;
SELECT pg_tde_delete_key();

-- Decrypt the table and delete the key
-- Should succeed: there is no more encrypted tables
ALTER TABLE test_table SET ACCESS METHOD heap;
SELECT pg_tde_delete_key();

-- Set local key, encrypt the table then delete teable and key
-- Should succeed: the table is deleted and there are no more encrypted tables
SELECT pg_tde_set_key_using_global_key_provider('test-db-key','file-provider');
ALTER TABLE test_table SET ACCESS METHOD tde_heap;
DROP TABLE test_table;
SELECT pg_tde_delete_key();

-- Set default key, set regular key, create table, delete regular key
-- Should succeed: regular key will be rotated to default key
SELECT pg_tde_set_default_key_using_global_key_provider('defalut-key','file-provider');
SELECT pg_tde_set_key_using_global_key_provider('test-db-key','file-provider');
CREATE TABLE test_table (id int, data text) USING tde_heap;
SELECT pg_tde_delete_key();
SELECT provider_id, provider_name, key_name FROM pg_tde_key_info();

-- Try to delete key when default key is used
-- Should fail: table already uses the default key, so there is no key to fallback to
SELECT pg_tde_delete_key();

-- Try to delete default key
-- Should fail: default key is used by the table
SELECT pg_tde_delete_default_key();

-- Set regular principal key, delete default key
-- Should succeed: the table will use the regular key
SELECT pg_tde_set_key_using_global_key_provider('test-db-key','file-provider');
SELECT pg_tde_delete_default_key();

DROP TABLE test_table;
SELECT pg_tde_delete_key();
SELECT pg_tde_delete_global_key_provider('file-provider');
DROP EXTENSION pg_tde;
