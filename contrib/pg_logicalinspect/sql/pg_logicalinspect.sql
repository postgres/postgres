CREATE EXTENSION pg_logicalinspect;

-- ===================================================================
-- Tests for input validation
-- ===================================================================

SELECT pg_get_logical_snapshot_info('0-40796E18.foo');
SELECT pg_get_logical_snapshot_info('0--40796E18.snap');
SELECT pg_get_logical_snapshot_info('-1--40796E18.snap');
SELECT pg_get_logical_snapshot_info('0/40796E18.foo.snap');
SELECT pg_get_logical_snapshot_info('0/40796E18.snap');
SELECT pg_get_logical_snapshot_info('');
SELECT pg_get_logical_snapshot_info(NULL);
SELECT pg_get_logical_snapshot_info('../snapshots');
SELECT pg_get_logical_snapshot_info('../snapshots/0-40796E18.snap');

SELECT pg_get_logical_snapshot_meta('0-40796E18.foo');
SELECT pg_get_logical_snapshot_meta('0-40796E18.foo.snap');
SELECT pg_get_logical_snapshot_meta('0/40796E18.snap');
SELECT pg_get_logical_snapshot_meta('');
SELECT pg_get_logical_snapshot_meta(NULL);
SELECT pg_get_logical_snapshot_meta('../snapshots');

-- ===================================================================
-- Tests for permissions
-- ===================================================================
CREATE ROLE regress_pg_logicalinspect;

SELECT has_function_privilege('regress_pg_logicalinspect',
  'pg_get_logical_snapshot_info(text)', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_logicalinspect',
  'pg_get_logical_snapshot_meta(text)', 'EXECUTE'); -- no

-- Functions accessible by users with role pg_read_server_files.
GRANT pg_read_server_files TO regress_pg_logicalinspect;

SELECT has_function_privilege('regress_pg_logicalinspect',
  'pg_get_logical_snapshot_info(text)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_logicalinspect',
  'pg_get_logical_snapshot_meta(text)', 'EXECUTE'); -- yes

-- ===================================================================
-- Clean up
-- ===================================================================

DROP ROLE regress_pg_logicalinspect;

DROP EXTENSION pg_logicalinspect;
