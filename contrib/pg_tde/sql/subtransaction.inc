CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');


BEGIN;                      -- Nesting level 1
SAVEPOINT sp;
CREATE TABLE foo(s TEXT);   -- Nesting level 2
RELEASE SAVEPOINT sp;
SAVEPOINT sp;
CREATE TABLE bar(s TEXT);   -- Nesting level 2
ROLLBACK TO sp;             -- Rollback should not affect first subtransaction
COMMIT;

BEGIN;                      -- Nesting level 1
SAVEPOINT sp;
DROP TABLE foo;             -- Nesting level 2
RELEASE SAVEPOINT sp;
SAVEPOINT sp;
CREATE TABLE bar(s TEXT);   -- Nesting level 2
ROLLBACK TO sp;             -- Rollback should not affect first subtransaction
COMMIT;

DROP EXTENSION pg_tde;