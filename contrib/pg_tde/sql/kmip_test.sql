CREATE EXTENSION pg_tde;

SELECT pg_tde_add_database_key_provider_kmip('kmip-prov','127.0.0.1', 5696, '/tmp/server_certificate.pem', '/tmp/client_certificate_jane_doe.pem', '/tmp/client_key_jane_doe.pem');
SELECT pg_tde_set_key_using_database_key_provider('kmip-key','kmip-prov');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING tde_heap;

INSERT INTO test_enc (k) VALUES (1);
INSERT INTO test_enc (k) VALUES (2);
INSERT INTO test_enc (k) VALUES (3);

SELECT * from test_enc;

SELECT pg_tde_verify_key();

DROP TABLE test_enc;

-- Creating provider fails if we can't connect to kmip server
SELECT pg_tde_add_database_key_provider_kmip('will-not-work','127.0.0.1', 61, '/tmp/server_certificate.pem', '/tmp/client_certificate_jane_doe.pem', '/tmp/client_key_jane_doe.pem');

DROP EXTENSION pg_tde;
