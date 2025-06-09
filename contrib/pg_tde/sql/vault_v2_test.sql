CREATE EXTENSION IF NOT EXISTS pg_tde;

\getenv root_token_file VAULT_ROOT_TOKEN_FILE
\getenv cacert_file VAULT_CACERT_FILE

SELECT pg_tde_add_database_key_provider_vault_v2('vault-incorrect', 'https://127.0.0.1:8200', 'DUMMY-TOKEN', :'root_token_file', :'cacert_file');
-- FAILS
SELECT pg_tde_set_key_using_database_key_provider('vault-v2-key', 'vault-incorrect');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING tde_heap;

SELECT pg_tde_add_database_key_provider_vault_v2('vault-v2', 'https://127.0.0.1:8200', 'secret', :'root_token_file', :'cacert_file');
SELECT pg_tde_set_key_using_database_key_provider('vault-v2-key', 'vault-v2');

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

-- Creating provider fails if we can't connect to vault
SELECT pg_tde_add_database_key_provider_vault_v2('will-not-work', 'https://127.0.0.1:61', 'secret', :'root_token_file', :'cacert_file');

-- Changing provider fails if we can't connect to vault
SELECT pg_tde_change_database_key_provider_vault_v2('vault-v2', 'https://127.0.0.1:61', 'secret', :'root_token_file', :'cacert_file');

-- HTTPS without cert fails
SELECT pg_tde_change_database_key_provider_vault_v2('vault-v2', 'https://127.0.0.1:8200', 'secret', :'root_token_file', NULL);

-- HTTP against HTTPS server fails
SELECT pg_tde_change_database_key_provider_vault_v2('vault-v2', 'http://127.0.0.1:8200', 'secret', :'root_token_file', NULL);

DROP EXTENSION pg_tde;
