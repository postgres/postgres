--
-- A composite datum that is incompatible with the current definition of its
-- type can be stored in a table (nothing deforms it on the way in).  Logical
-- decoding of the transaction that stored it then fails, as the output plugin
-- deforms the row with the current descriptor, and the slot cannot advance
-- past that transaction.  This shows that behaviour.
--

-- predictability
SET synchronous_commit = on;

-- the decoding errors' CONTEXT includes an LSN
\set VERBOSITY terse

CREATE TYPE ld_type AS (a bytea, i int);
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

-- Form a datum, alter the type underneath it in a later transaction, then
-- store the stale datum.
CREATE PROCEDURE ld_store() LANGUAGE plpgsql AS $$
DECLARE v ld_type;
BEGIN
  v := '(\\x4649584d45,1)'::ld_type;
  COMMIT;
  ALTER TYPE ld_type ALTER ATTRIBUTE a TYPE text;
  CREATE TABLE ld_tbl (id int, c ld_type);
  COMMIT;
  INSERT INTO ld_tbl VALUES (1, v);
END $$;
CALL ld_store();

-- Reading the stored datum fails ...
SELECT (c).a FROM ld_tbl;

-- ... and so does decoding the transaction that stored it.
SELECT count(*) FROM pg_logical_slot_peek_changes('regression_slot', NULL, NULL, 'skip-empty-xacts', '1');

-- Fixing the row does not help: The change record is already in the WAL.
UPDATE ld_tbl SET c = ROW('fixed', 2)::ld_type;
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'skip-empty-xacts', '1');

-- Advancing the slot past the transaction is the way out.
SELECT pg_replication_slot_advance('regression_slot', pg_current_wal_lsn()) IS NOT NULL;
SELECT count(*) FROM pg_logical_slot_get_changes('regression_slot', NULL, NULL, 'skip-empty-xacts', '1');

SELECT pg_drop_replication_slot('regression_slot');
DROP TABLE ld_tbl;
DROP PROCEDURE ld_store();
DROP TYPE ld_type;
