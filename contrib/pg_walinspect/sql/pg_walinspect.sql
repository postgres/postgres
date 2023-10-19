CREATE EXTENSION pg_walinspect;

-- Mask DETAIL messages as these could refer to current LSN positions.
\set VERBOSITY terse

-- Make sure checkpoints don't interfere with the test.
SELECT 'init' FROM pg_create_physical_replication_slot('regress_pg_walinspect_slot', true, false);

CREATE TABLE sample_tbl(col1 int, col2 int);

-- Save some LSNs for comparisons.
SELECT pg_current_wal_lsn() AS wal_lsn1 \gset
INSERT INTO sample_tbl SELECT * FROM generate_series(1, 2);
SELECT pg_current_wal_lsn() AS wal_lsn2 \gset
INSERT INTO sample_tbl SELECT * FROM generate_series(3, 4);

-- ===================================================================
-- Tests for input validation
-- ===================================================================

-- Invalid input LSN.
SELECT * FROM pg_get_wal_record_info('0/0');

-- Invalid start LSN.
SELECT * FROM pg_get_wal_records_info('0/0', :'wal_lsn1');
SELECT * FROM pg_get_wal_stats('0/0', :'wal_lsn1');
SELECT * FROM pg_get_wal_block_info('0/0', :'wal_lsn1');

-- Start LSN > End LSN.
SELECT * FROM pg_get_wal_records_info(:'wal_lsn2', :'wal_lsn1');
SELECT * FROM pg_get_wal_stats(:'wal_lsn2', :'wal_lsn1');
SELECT * FROM pg_get_wal_block_info(:'wal_lsn2', :'wal_lsn1');

-- LSNs with the highest value possible.
SELECT * FROM pg_get_wal_record_info('FFFFFFFF/FFFFFFFF');
-- Success with end LSNs.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', 'FFFFFFFF/FFFFFFFF');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_stats(:'wal_lsn1', 'FFFFFFFF/FFFFFFFF');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn1', 'FFFFFFFF/FFFFFFFF');
-- Failures with start LSNs.
SELECT * FROM pg_get_wal_records_info('FFFFFFFF/FFFFFFFE', 'FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_stats('FFFFFFFF/FFFFFFFE', 'FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_block_info('FFFFFFFF/FFFFFFFE', 'FFFFFFFF/FFFFFFFF');

-- ===================================================================
-- Tests for all function executions
-- ===================================================================

SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_record_info(:'wal_lsn1');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_stats(:'wal_lsn1', :'wal_lsn2');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn1', :'wal_lsn2');

-- ===================================================================
-- Test for filtering out WAL records of a particular table
-- ===================================================================

SELECT oid AS sample_tbl_oid FROM pg_class WHERE relname = 'sample_tbl' \gset

SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2')
			WHERE block_ref LIKE concat('%', :'sample_tbl_oid', '%') AND resource_manager = 'Heap';

-- ===================================================================
-- Test for filtering out WAL records based on resource_manager and
-- record_type
-- ===================================================================

SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2')
			WHERE resource_manager = 'Heap' AND record_type = 'INSERT';

-- ===================================================================
-- Tests to get block information from WAL record
-- ===================================================================

-- Update table to generate some block data.
SELECT pg_current_wal_lsn() AS wal_lsn3 \gset
UPDATE sample_tbl SET col1 = col1 + 1 WHERE col1 = 1;
SELECT pg_current_wal_lsn() AS wal_lsn4 \gset
-- Check if we get block data from WAL record.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn3', :'wal_lsn4')
  WHERE relfilenode = :'sample_tbl_oid' AND block_data IS NOT NULL;

-- Force a checkpoint so that the next update will log a full-page image.
SELECT pg_current_wal_lsn() AS wal_lsn5 \gset
CHECKPOINT;

-- Verify that an XLOG_CHECKPOINT_REDO record begins at precisely the redo LSN
-- of the checkpoint we just performed.
SELECT redo_lsn FROM pg_control_checkpoint() \gset
SELECT start_lsn = :'redo_lsn'::pg_lsn AS same_lsn, resource_manager,
    record_type FROM pg_get_wal_record_info(:'redo_lsn');

-- This update should produce a full-page image because of the checkpoint.
UPDATE sample_tbl SET col1 = col1 + 1 WHERE col1 = 2;
SELECT pg_current_wal_lsn() AS wal_lsn6 \gset
-- Check if we get FPI from WAL record.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn5', :'wal_lsn6')
  WHERE relfilenode = :'sample_tbl_oid' AND block_fpi_data IS NOT NULL;

-- ===================================================================
-- Tests for permissions
-- ===================================================================
CREATE ROLE regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- no

-- Functions accessible by users with role pg_read_server_files.
GRANT pg_read_server_files TO regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes

REVOKE pg_read_server_files FROM regress_pg_walinspect;

-- Superuser can grant execute to other users.
GRANT EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean)
  TO regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes

REVOKE EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean)
  FROM regress_pg_walinspect;

-- ===================================================================
-- Clean up
-- ===================================================================

DROP ROLE regress_pg_walinspect;

SELECT pg_drop_replication_slot('regress_pg_walinspect_slot');

DROP TABLE sample_tbl;
DROP EXTENSION pg_walinspect;
