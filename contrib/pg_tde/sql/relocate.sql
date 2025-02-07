-- Support pg_tde already being installed
SET client_min_messages = 'warning';
DROP EXTENSION IF EXISTS pg_tde;

CREATE SCHEMA other;

CREATE EXTENSION pg_tde SCHEMA other;

SELECT other.pg_tde_add_key_provider_file('file-vault', '/tmp/pg_tde_test_keyring.per');

SELECT other.pg_tde_grant_key_viewer_to_role('public');

ALTER EXTENSION pg_tde SET SCHEMA public;

DROP EXTENSION pg_tde;

DROP SCHEMA other;
