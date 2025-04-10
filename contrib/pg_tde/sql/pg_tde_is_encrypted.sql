CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT * FROM pg_tde_key_info();

SELECT pg_tde_add_database_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-vault');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING tde_heap;

CREATE TABLE test_norm(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING heap;

CREATE TABLE test_part(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) PARTITION BY RANGE (id) USING tde_heap;

SELECT relname, amname FROM pg_class JOIN pg_am ON pg_am.oid = pg_class.relam WHERE relname IN ('test_enc', 'test_norm', 'test_part') ORDER BY relname;

SELECT relname, pg_tde_is_encrypted(relname) FROM (VALUES ('test_enc'), ('test_norm'), ('test_part')) AS r (relname) ORDER BY relname;

SELECT relname, pg_tde_is_encrypted(relname) FROM (VALUES ('test_enc_id_seq'), ('test_norm_id_seq'), ('test_part_id_seq')) AS r (relname) ORDER BY relname;

SELECT relname, pg_tde_is_encrypted(relname) FROM (VALUES ('test_enc_pkey'), ('test_norm_pkey'), ('test_part_pkey')) AS r (relname) ORDER BY relname;

SELECT pg_tde_is_encrypted(NULL);

SELECT key_provider_id, key_provider_name, key_name
    FROM pg_tde_key_info();

DROP TABLE test_part;
DROP TABLE test_norm;
DROP TABLE test_enc;

DROP EXTENSION pg_tde;
