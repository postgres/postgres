-- predictability
SET synchronous_commit = on;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

CREATE SEQUENCE test_sequence;

-- test the sequence changes by several nextval() calls
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');

-- test the sequence changes by several ALTER commands
ALTER SEQUENCE test_sequence INCREMENT BY 10;
SELECT nextval('test_sequence');

ALTER SEQUENCE test_sequence START WITH 3000;
ALTER SEQUENCE test_sequence MAXVALUE 10000;
ALTER SEQUENCE test_sequence RESTART WITH 4000;
SELECT nextval('test_sequence');

-- test the sequence changes by several setval() calls
SELECT setval('test_sequence', 3500);
SELECT nextval('test_sequence');
SELECT setval('test_sequence', 3500, true);
SELECT nextval('test_sequence');
SELECT setval('test_sequence', 3500, false);
SELECT nextval('test_sequence');

-- show results and drop sequence
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
DROP SEQUENCE test_sequence;

-- rollback on sequence creation and update
BEGIN;
CREATE SEQUENCE test_sequence;
CREATE TABLE test_table (a INT);
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');
SELECT setval('test_sequence', 3000);
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');
SELECT nextval('test_sequence');
ALTER SEQUENCE test_sequence RESTART WITH 6000;
INSERT INTO test_table VALUES( (SELECT nextval('test_sequence')) );
SELECT nextval('test_sequence');
ROLLBACK;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- rollback on table creation with serial column
BEGIN;
CREATE TABLE test_table (a SERIAL, b INT);
INSERT INTO test_table (b) VALUES (100);
INSERT INTO test_table (b) VALUES (200);
INSERT INTO test_table (b) VALUES (300);
SELECT setval('test_table_a_seq', 3000);
INSERT INTO test_table (b) VALUES (400);
INSERT INTO test_table (b) VALUES (500);
INSERT INTO test_table (b) VALUES (600);
ALTER SEQUENCE test_table_a_seq RESTART WITH 6000;
INSERT INTO test_table (b) VALUES (700);
INSERT INTO test_table (b) VALUES (800);
INSERT INTO test_table (b) VALUES (900);
ROLLBACK;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- rollback on table with serial column
CREATE TABLE test_table (a SERIAL, b INT);

BEGIN;
INSERT INTO test_table (b) VALUES (100);
INSERT INTO test_table (b) VALUES (200);
INSERT INTO test_table (b) VALUES (300);
SELECT setval('test_table_a_seq', 3000);
INSERT INTO test_table (b) VALUES (400);
INSERT INTO test_table (b) VALUES (500);
INSERT INTO test_table (b) VALUES (600);
ALTER SEQUENCE test_table_a_seq RESTART WITH 6000;
INSERT INTO test_table (b) VALUES (700);
INSERT INTO test_table (b) VALUES (800);
INSERT INTO test_table (b) VALUES (900);
ROLLBACK;

-- check table and sequence values after rollback
SELECT * from test_table_a_seq;
SELECT nextval('test_table_a_seq');

DROP TABLE test_table;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- savepoint test on table with serial column
BEGIN;
CREATE TABLE test_table (a SERIAL, b INT);
INSERT INTO test_table (b) VALUES (100);
INSERT INTO test_table (b) VALUES (200);
SAVEPOINT a;
INSERT INTO test_table (b) VALUES (300);
ROLLBACK TO SAVEPOINT a;
DROP TABLE test_table;
COMMIT;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- savepoint test on table with serial column
BEGIN;
CREATE SEQUENCE test_sequence;
SELECT nextval('test_sequence');
SELECT setval('test_sequence', 3000);
SELECT nextval('test_sequence');
SAVEPOINT a;
ALTER SEQUENCE test_sequence START WITH 7000;
SELECT setval('test_sequence', 5000);
ROLLBACK TO SAVEPOINT a;
SELECT * FROM test_sequence;
DROP SEQUENCE test_sequence;
COMMIT;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT pg_drop_replication_slot('regression_slot');
