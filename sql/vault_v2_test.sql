CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_vault_v2('vault-v2','ROOT_TOKEN','http://127.0.0.1:8200','secret',NULL);
SELECT pg_tde_set_master_key('vault-v2-master-key','vault-v2');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING pg_tde;

INSERT INTO test_enc (k) VALUES (1);
INSERT INTO test_enc (k) VALUES (2);
INSERT INTO test_enc (k) VALUES (3);

SELECT * from test_enc;

DROP TABLE test_enc;

DROP EXTENSION pg_tde;
