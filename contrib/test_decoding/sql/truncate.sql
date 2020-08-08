-- predictability
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

CREATE TABLE tab1 (id serial unique, data int);
CREATE TABLE tab2 (a int primary key, b int);

TRUNCATE tab1;
TRUNCATE tab1, tab1 RESTART IDENTITY CASCADE;
TRUNCATE tab1, tab2;

SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
SELECT pg_drop_replication_slot('regression_slot');
