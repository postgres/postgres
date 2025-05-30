CREATE EXTENSION IF NOT EXISTS pg_tde;

\getenv root_token_file VAULT_ROOT_TOKEN_FILE
\getenv cacert_file VAULT_CACERT_FILE

SELECT pg_tde_add_database_key_provider_vault_v2('vault-incorrect',:'root_token_file','https://127.0.0.1:8200','DUMMY-TOKEN',:'cacert_file');
-- FAILS
SELECT pg_tde_set_key_using_database_key_provider('vault-v2-key','vault-incorrect');

CREATE TABLE test_enc(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING tde_heap;

SELECT pg_tde_add_database_key_provider_vault_v2('vault-v2',:'root_token_file','https://127.0.0.1:8200','secret',:'cacert_file');
SELECT pg_tde_set_key_using_database_key_provider('vault-v2-key','vault-v2');

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
SELECT pg_tde_add_database_key_provider_vault_v2('will-not-work', :'root_token_file', 'https://127.0.0.1:61', 'secret', :'cacert_file');

-- Changing provider fails if we can't connect to vault
SELECT pg_tde_change_database_key_provider_vault_v2('vault-v2', :'root_token_file', 'https://127.0.0.1:61', 'secret', :'cacert_file');

-- HTTPS without cert fails
SELECT pg_tde_change_database_key_provider_vault_v2('vault-v2', :'root_token_file', 'https://127.0.0.1:8200', 'secret', NULL);

-- HTTP against HTTPS server fails
SELECT pg_tde_change_database_key_provider_vault_v2('vault-v2', :'root_token_file', 'http://127.0.0.1:8200', 'secret', NULL);

DROP EXTENSION pg_tde;
