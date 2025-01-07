CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');


CREATE TABLE update_test (
    a   INT DEFAULT 10,
    b   INT,
    c   TEXT
) USING tde_heap_basic;

CREATE TABLE upsert_test (
    a   INT PRIMARY KEY,
    b   TEXT
) USING tde_heap_basic;

INSERT INTO update_test VALUES (5, 10, 'foo');
INSERT INTO update_test(b, a) VALUES (15, 10);

INSERT INTO upsert_test VALUES (2, 'Beeble') ON CONFLICT(a)
  DO UPDATE SET (b, a) = (SELECT b || ', Excluded', a from upsert_test i WHERE i.a = excluded.a)
  RETURNING tableoid::regclass, xmin = pg_current_xact_id()::xid AS xmin_correct, xmax = 0 AS xmax_correct;
-- currently xmax is set after a conflict - that's probably not good,
-- but it seems worthwhile to have to be explicit if that changes.
INSERT INTO upsert_test VALUES (2, 'Brox') ON CONFLICT(a)
  DO UPDATE SET (b, a) = (SELECT b || ', Excluded', a from upsert_test i WHERE i.a = excluded.a)
  RETURNING tableoid::regclass, xmin = pg_current_xact_id()::xid AS xmin_correct, xmax = pg_current_xact_id()::xid AS xmax_correct;

DROP TABLE update_test;
DROP TABLE upsert_test;

DROP EXTENSION pg_tde;