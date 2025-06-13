CREATE SCHEMA other;

CREATE EXTENSION pg_tde SCHEMA other;

SELECT other.pg_tde_add_database_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');

ALTER EXTENSION pg_tde SET SCHEMA public;

DROP EXTENSION pg_tde;

DROP SCHEMA other;
