CREATE EXTENSION IF NOT EXISTS pg_tde;

CREATE DATABASE template_db;

SELECT current_database() AS regress_database
\gset

\c template_db

CREATE EXTENSION pg_tde;

SELECT pg_tde_add_database_key_provider_file('file-vault', '/tmp/template_provider.per');
SELECT pg_tde_set_key_using_database_key_provider('test-db-key', 'file-vault');

CREATE TABLE test_enc (id serial PRIMARY KEY, x int) USING tde_heap;
CREATE TABLE test_plain (id serial PRIMARY KEY, x int) USING heap;

INSERT INTO test_enc (x) VALUES (10), (20);
INSERT INTO test_plain (x) VALUES (30), (40);

\c :regress_database

-- TODO: Test the case where we have no default key once we can delete default keys
--CREATE DATABASE new_db TEMPLATE template_db;

SELECT pg_tde_add_global_key_provider_file('global-file-vault','/tmp/template_provider_global.per');

SELECT pg_tde_set_default_key_using_global_key_provider('default-key', 'global-file-vault');

CREATE DATABASE new_db TEMPLATE template_db;

\c new_db

INSERT INTO test_enc (x) VALUES (25);
SELECT * FROM test_enc;
SELECT pg_tde_is_encrypted('test_enc');
SELECT pg_tde_is_encrypted('test_enc_pkey');
SELECT pg_tde_is_encrypted('test_enc_id_seq');

INSERT INTO test_plain (x) VALUES (45);
SELECT * FROM test_plain;
SELECT pg_tde_is_encrypted('test_plain');
SELECT pg_tde_is_encrypted('test_plain_pkey');
SELECT pg_tde_is_encrypted('test_plain_id_seq');

\c :regress_database

CREATE DATABASE new_db_file_copy TEMPLATE template_db STRATEGY FILE_COPY;

\c template_db

DROP TABLE test_enc;

\c :regress_database

CREATE DATABASE new_db_file_copy TEMPLATE template_db STRATEGY FILE_COPY;

DROP DATABASE new_db_file_copy;
DROP DATABASE new_db;
DROP DATABASE template_db;

DROP EXTENSION pg_tde;
