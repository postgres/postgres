CREATE EXTENSION injection_points;

SELECT injection_points_attach('TestInjectionBooh', 'booh');
SELECT injection_points_attach('TestInjectionError', 'error');
SELECT injection_points_attach('TestInjectionLog', 'notice');
SELECT injection_points_attach('TestInjectionLog2', 'notice');

SELECT injection_points_run('TestInjectionBooh'); -- nothing
SELECT injection_points_run('TestInjectionLog2'); -- notice
SELECT injection_points_run('TestInjectionLog'); -- notice
SELECT injection_points_run('TestInjectionError'); -- error

-- Re-load cache and run again.
\c
SELECT injection_points_run('TestInjectionLog2'); -- notice
SELECT injection_points_run('TestInjectionLog'); -- notice
SELECT injection_points_run('TestInjectionError'); -- error

-- Remove one entry and check the remaining entries.
SELECT injection_points_detach('TestInjectionError'); -- ok
SELECT injection_points_run('TestInjectionLog'); -- notice
SELECT injection_points_run('TestInjectionError'); -- nothing
-- More entries removed, letting TestInjectionLog2 to check the same
-- callback used in more than one point.
SELECT injection_points_detach('TestInjectionLog'); -- ok
SELECT injection_points_run('TestInjectionLog'); -- nothing
SELECT injection_points_run('TestInjectionError'); -- nothing
SELECT injection_points_run('TestInjectionLog2'); -- notice

SELECT injection_points_detach('TestInjectionLog'); -- fails

SELECT injection_points_run('TestInjectionLog2'); -- notice

DROP EXTENSION injection_points;
