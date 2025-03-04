CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_kmip('kmip-prov','127.0.0.1', 5696, '/tmp/server_certificate.pem', '/tmp/client_key_jane_doe.pem');
SELECT pg_tde_set_principal_key('kmip-principal-key','kmip-prov');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1);
INSERT INTO test_enc (k) VALUES (2);
INSERT INTO test_enc (k) VALUES (3);

SELECT * from test_enc;

DROP TABLE test_enc;

DROP EXTENSION pg_tde;
