\! rm -f '/tmp/pg_tde_regression_default_key.per'

CREATE EXTENSION IF NOT EXISTS pg_tde;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

SELECT pg_tde_add_global_key_provider_file('file-provider','/tmp/pg_tde_regression_default_key.per');

-- Should fail: no default principal key for the server yet
SELECT pg_tde_verify_default_key();

-- Should fail: no default principal key for the server yet
SELECT provider_id, provider_name, key_name
		FROM pg_tde_default_key_info();

SELECT pg_tde_set_default_key_using_global_key_provider('default-key', 'file-provider', false);
SELECT pg_tde_verify_default_key();

SELECT provider_id, provider_name, key_name
		FROM pg_tde_default_key_info();

-- fails
SELECT pg_tde_delete_global_key_provider('file-provider');
SELECT id, name FROM pg_tde_list_all_global_key_providers();

-- Should fail: no principal key for the database yet
SELECT  provider_id, provider_name, key_name
		FROM pg_tde_key_info();

-- Should succeed: "localizes" the default principal key for the database
CREATE TABLE test_enc(
	id SERIAL,
	k INTEGER DEFAULT '0' NOT NULL,
	PRIMARY KEY (id)
) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1), (2), (3);

-- Should succeed: create table localized the principal key
SELECT  provider_id, provider_name, key_name
		FROM pg_tde_key_info();

SELECT current_database() AS regress_database
\gset

CREATE DATABASE regress_pg_tde_other;

\c regress_pg_tde_other

CREATE EXTENSION pg_tde;
CREATE EXTENSION pg_buffercache;

-- Should fail: no principal key for the database yet
SELECT  provider_id, provider_name, key_name
		FROM pg_tde_key_info();

-- Should succeed: "localizes" the default principal key for the database
CREATE TABLE test_enc(
	id SERIAL,
	k INTEGER DEFAULT '0' NOT NULL,
	PRIMARY KEY (id)
) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1), (2), (3);

-- Should succeed: create table localized the principal key
SELECT  provider_id, provider_name, key_name
		FROM pg_tde_key_info();

\c :regress_database

CHECKPOINT;

SELECT pg_tde_set_default_key_using_global_key_provider('new-default-key', 'file-provider', false);

SELECT  provider_id, provider_name, key_name
		FROM pg_tde_key_info();

\c regress_pg_tde_other

SELECT  provider_id, provider_name, key_name
		FROM pg_tde_key_info();

SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache WHERE relfilenode = (SELECT relfilenode FROM pg_class WHERE oid = 'test_enc'::regclass);

SELECT * FROM test_enc;

DROP TABLE test_enc;

DROP EXTENSION pg_tde CASCADE;

\c :regress_database

SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache WHERE relfilenode = (SELECT relfilenode FROM pg_class WHERE oid = 'test_enc'::regclass);

SELECT * FROM test_enc;

DROP TABLE test_enc;
SELECT pg_tde_delete_default_key();
SELECT pg_tde_delete_global_key_provider('file-provider');
DROP EXTENSION pg_tde CASCADE;
DROP EXTENSION pg_buffercache;

DROP DATABASE regress_pg_tde_other;
