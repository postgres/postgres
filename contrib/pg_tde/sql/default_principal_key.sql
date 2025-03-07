CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT pg_tde_add_global_key_provider_file('file-provider','/tmp/pg_tde_regression_default_principal_key.per');

SELECT pg_tde_set_default_principal_key('default-principal-key', 'file-provider', false);

-- fails
SELECT pg_tde_delete_global_key_provider('file-provider');
SELECT id, provider_name FROM pg_tde_list_all_global_key_providers();

-- Should fail: no principal key for the database yet
SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();
 
-- Should succeed: "localizes" the default principal key for the database
CREATE TABLE test_enc(
	id SERIAL,
	k INTEGER DEFAULT '0' NOT NULL,
	PRIMARY KEY (id)
) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1), (2), (3);

-- Should succeed: create table localized the principal key
SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();

SELECT current_database() AS regress_database
\gset

CREATE DATABASE regress_pg_tde_other;

\c regress_pg_tde_other

CREATE EXTENSION pg_tde;

-- Should fail: no principal key for the database yet
SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();

-- Should succeed: "localizes" the default principal key for the database
CREATE TABLE test_enc(
	id SERIAL,
	k INTEGER DEFAULT '0' NOT NULL,
	PRIMARY KEY (id)
) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1), (2), (3);

-- Should succeed: create table localized the principal key
SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();

\c :regress_database

SELECT pg_tde_set_default_principal_key('new-default-principal-key', 'file-provider', false);

SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();

\c regress_pg_tde_other

SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();

DROP TABLE test_enc;

DROP EXTENSION pg_tde CASCADE;

\c :regress_database

DROP TABLE test_enc;

DROP EXTENSION pg_tde CASCADE;

DROP DATABASE regress_pg_tde_other;
