-- Tests for VACUUM

CREATE EXTENSION injection_points;

SELECT injection_points_set_local();
SELECT injection_points_attach('vacuum-index-cleanup-auto', 'notice');
SELECT injection_points_attach('vacuum-index-cleanup-disabled', 'notice');
SELECT injection_points_attach('vacuum-index-cleanup-enabled', 'notice');
SELECT injection_points_attach('vacuum-truncate-auto', 'notice');
SELECT injection_points_attach('vacuum-truncate-disabled', 'notice');
SELECT injection_points_attach('vacuum-truncate-enabled', 'notice');

-- Check state of index_cleanup and truncate in VACUUM.
CREATE TABLE vac_tab_on_toast_off(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=true, toast.vacuum_index_cleanup=false,
   vacuum_truncate=true, toast.vacuum_truncate=false);
CREATE TABLE vac_tab_off_toast_on(i int, j text) WITH
  (autovacuum_enabled=false,
   vacuum_index_cleanup=false, toast.vacuum_index_cleanup=true,
   vacuum_truncate=false, toast.vacuum_truncate=true);
-- Multiple relations should use their options in isolation.
VACUUM vac_tab_on_toast_off, vac_tab_off_toast_on;

DROP TABLE vac_tab_on_toast_off;
DROP TABLE vac_tab_off_toast_on;

-- Cleanup
SELECT injection_points_detach('vacuum-index-cleanup-auto');
SELECT injection_points_detach('vacuum-index-cleanup-disabled');
SELECT injection_points_detach('vacuum-index-cleanup-enabled');
SELECT injection_points_detach('vacuum-truncate-auto');
SELECT injection_points_detach('vacuum-truncate-disabled');
SELECT injection_points_detach('vacuum-truncate-enabled');
DROP EXTENSION injection_points;
