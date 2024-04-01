CREATE EXTENSION pg_tde;

SELECT  * FROM pg_tde_master_key_info();

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_master_key('test-db-master-key','file-vault');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING pg_tde;

CREATE TABLE test_norm(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING heap;

SELECT amname FROM pg_class INNER JOIN pg_am ON pg_am.oid = pg_class.relam WHERE relname = 'test_enc';
SELECT amname FROM pg_class INNER JOIN pg_am ON pg_am.oid = pg_class.relam WHERE relname = 'test_norm';

SELECT pgtde_is_encrypted('test_enc');
SELECT pgtde_is_encrypted('test_norm');

SELECT  key_provider_id, key_provider_name, master_key_name
		FROM pg_tde_master_key_info();

DROP TABLE test_enc;
DROP TABLE test_norm;

DROP EXTENSION pg_tde;
