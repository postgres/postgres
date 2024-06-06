-- predictability
SET synchronous_commit = on;

-- fail because we're creating a slot while in an xact with xid
BEGIN;
SELECT pg_current_xact_id() = '0';
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
ROLLBACK;

-- fail because we're creating a slot while in a subxact whose topxact has an xid
BEGIN;
SELECT pg_current_xact_id() = '0';
SAVEPOINT barf;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
ROLLBACK TO SAVEPOINT barf;
ROLLBACK;

-- succeed, outside tx.
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
SELECT 'stop' FROM pg_drop_replication_slot('regression_slot');

-- succeed, in tx without xid.
BEGIN;
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');
COMMIT;

CREATE TABLE nobarf(id serial primary key, data text);
INSERT INTO nobarf(data) VALUES('1');

-- decoding works in transaction with xid
BEGIN;
SELECT pg_current_xact_id() = '0';
-- don't show yet, haven't committed
INSERT INTO nobarf(data) VALUES('2');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
COMMIT;

INSERT INTO nobarf(data) VALUES('3');
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

-- Decoding works in transaction that issues DDL
--
-- We had issues handling relcache invalidations with these, see
-- https://www.postgresql.org/message-id/e56be7d9-14b1-664d-0bfc-00ce9772721c@gmail.com
CREATE TABLE tbl_created_outside_xact(id SERIAL PRIMARY KEY);
BEGIN;
  -- TRUNCATE changes the relfilenode and sends relcache invalidation
  TRUNCATE tbl_created_outside_xact;
  INSERT INTO tbl_created_outside_xact(id) VALUES('1');

  -- don't show yet, haven't committed
  SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
COMMIT;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SET debug_logical_replication_streaming = immediate;
BEGIN;
  CREATE TABLE tbl_created_in_xact(id SERIAL PRIMARY KEY);
  INSERT INTO tbl_created_in_xact VALUES (1);

  CHECKPOINT; -- Force WAL flush, so that the above changes will be streamed

  SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'stream-changes', '1');
COMMIT;
RESET debug_logical_replication_streaming;
SELECT data FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');

SELECT 'stop' FROM pg_drop_replication_slot('regression_slot');
