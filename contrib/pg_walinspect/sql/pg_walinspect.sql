CREATE EXTENSION pg_walinspect;

-- Make sure checkpoints don't interfere with the test.
SELECT 'init' FROM pg_create_physical_replication_slot('regress_pg_walinspect_slot', true, false);

CREATE TABLE sample_tbl(col1 int, col2 int);

SELECT pg_current_wal_lsn() AS wal_lsn1 \gset

INSERT INTO sample_tbl SELECT * FROM generate_series(1, 2);

SELECT pg_current_wal_lsn() AS wal_lsn2 \gset

INSERT INTO sample_tbl SELECT * FROM generate_series(1, 2);

-- ===================================================================
-- Tests for input validation
-- ===================================================================

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_records_info(:'wal_lsn2', :'wal_lsn1'); -- ERROR

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_stats(:'wal_lsn2', :'wal_lsn1'); -- ERROR

-- ===================================================================
-- Tests for all function executions
-- ===================================================================

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_record_info(:'wal_lsn1');

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2');

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_records_info_till_end_of_wal(:'wal_lsn1');

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_stats(:'wal_lsn1', :'wal_lsn2');

SELECT COUNT(*) >= 0 AS ok FROM pg_get_wal_stats_till_end_of_wal(:'wal_lsn1');

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
-- Tests for permissions
-- ===================================================================
CREATE ROLE regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- no

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- no

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- no

-- Functions accessible by users with role pg_read_server_files

GRANT pg_read_server_files TO regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- yes

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- yes

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes

REVOKE pg_read_server_files FROM regress_pg_walinspect;

-- Superuser can grant execute to other users
GRANT EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn)
  TO regress_pg_walinspect;

GRANT EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn)
  TO regress_pg_walinspect;

GRANT EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean)
  TO regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- yes

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- yes

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes

REVOKE EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn)
  FROM regress_pg_walinspect;

REVOKE EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn)
  FROM regress_pg_walinspect;

REVOKE EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean)
  FROM regress_pg_walinspect;

-- ===================================================================
-- Clean up
-- ===================================================================

DROP ROLE regress_pg_walinspect;

SELECT pg_drop_replication_slot('regress_pg_walinspect_slot');

DROP TABLE sample_tbl;
