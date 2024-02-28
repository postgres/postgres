CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_master_key('test-db-master-key','file-vault');

CREATE TABLE src (f1 TEXT STORAGE EXTERNAL) USING pg_tde;
INSERT INTO src VALUES(repeat('abcdeF',1000));
SELECT * FROM src;

DROP TABLE src;

DROP EXTENSION pg_tde;
