CREATE EXTENSION pg_tde;

SELECT  * FROM pg_tde_principal_key_info();

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING tde_heap_basic;

CREATE TABLE test_norm(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING heap;

SELECT amname FROM pg_class INNER JOIN pg_am ON pg_am.oid = pg_class.relam WHERE relname = 'test_enc';
SELECT amname FROM pg_class INNER JOIN pg_am ON pg_am.oid = pg_class.relam WHERE relname = 'test_norm';

SELECT pg_tde_is_encrypted('test_enc');
SELECT pg_tde_is_encrypted('test_norm');

SELECT  key_provider_id, key_provider_name, principal_key_name
		FROM pg_tde_principal_key_info();

DROP TABLE test_enc;
DROP TABLE test_norm;

DROP EXTENSION pg_tde;
