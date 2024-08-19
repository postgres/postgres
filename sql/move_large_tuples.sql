-- test pg_tde_move_encrypted_data()
CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');

CREATE TABLE sbtest2(
	  id SERIAL,
	  k TEXT STORAGE PLAIN,
	  PRIMARY KEY (id)
	) USING tde_heap_basic;

INSERT INTO sbtest2(k) VALUES(repeat('a', 2500));
INSERT INTO sbtest2(k) VALUES(repeat('b', 2500));
INSERT INTO sbtest2(k) VALUES(repeat('c', 2500));
INSERT INTO sbtest2(k) VALUES(repeat('d', 2500));
INSERT INTO sbtest2(k) VALUES(repeat('e', 2500));

DELETE FROM sbtest2 WHERE id IN (2,3,4);
VACUUM sbtest2;
SELECT * FROM sbtest2;

INSERT INTO sbtest2(k) VALUES(repeat('b', 2500));
INSERT INTO sbtest2(k) VALUES(repeat('c', 2500));
INSERT INTO sbtest2(k) VALUES(repeat('d', 2500));

DELETE FROM sbtest2 WHERE id IN (7);
VACUUM sbtest2;

SELECT * FROM sbtest2;

VACUUM FULL sbtest2;
SELECT * FROM sbtest2;

DROP TABLE sbtest2;
DROP EXTENSION pg_tde;
