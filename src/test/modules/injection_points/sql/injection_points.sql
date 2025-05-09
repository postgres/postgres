CREATE EXTENSION injection_points;

\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX
\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION wait_pid(int)
  RETURNS void
  AS :'regresslib'
  LANGUAGE C STRICT;

-- Non-strict checks
SELECT injection_points_run(NULL);
SELECT injection_points_cached(NULL);

SELECT injection_points_attach('TestInjectionBooh', 'booh');
SELECT injection_points_attach('TestInjectionError', 'error');
SELECT injection_points_attach('TestInjectionLog', 'notice');
SELECT injection_points_attach('TestInjectionLog2', 'notice');

SELECT injection_points_run('TestInjectionBooh'); -- nothing
SELECT injection_points_run('TestInjectionLog2'); -- notice
SELECT injection_points_run('TestInjectionLog2', NULL); -- notice
SELECT injection_points_run('TestInjectionLog2', 'foobar'); -- notice + arg
SELECT injection_points_run('TestInjectionLog'); -- notice
SELECT injection_points_run('TestInjectionError'); -- error
SELECT injection_points_run('TestInjectionError', NULL); -- error
SELECT injection_points_run('TestInjectionError', 'foobar2'); -- error + arg

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
SELECT injection_points_detach('TestInjectionLog2');

-- Loading
SELECT injection_points_cached('TestInjectionLogLoad'); -- nothing in cache
SELECT injection_points_load('TestInjectionLogLoad'); -- nothing
SELECT injection_points_attach('TestInjectionLogLoad', 'notice');
SELECT injection_points_load('TestInjectionLogLoad'); -- nothing happens
SELECT injection_points_cached('TestInjectionLogLoad'); -- runs from cache
SELECT injection_points_cached('TestInjectionLogLoad', NULL); -- runs from cache
SELECT injection_points_cached('TestInjectionLogLoad', 'foobar'); -- runs from cache
SELECT injection_points_run('TestInjectionLogLoad'); -- runs from cache
SELECT injection_points_detach('TestInjectionLogLoad');

-- Runtime conditions
SELECT injection_points_attach('TestConditionError', 'error');
-- Any follow-up injection point attached will be local to this process.
SELECT injection_points_set_local();
SELECT injection_points_attach('TestConditionLocal1', 'error');
SELECT injection_points_attach('TestConditionLocal2', 'notice');
SELECT injection_points_run('TestConditionLocal1'); -- error
SELECT injection_points_run('TestConditionLocal2'); -- notice

SELECT pg_backend_pid() AS oldpid \gset

-- reload, local injection points should be gone.
\c
-- Wait for the previous backend process to exit, ensuring that its local
-- injection points are cleaned up.
SELECT wait_pid(:'oldpid');
SELECT injection_points_run('TestConditionLocal1'); -- nothing
SELECT injection_points_run('TestConditionLocal2'); -- nothing
SELECT injection_points_run('TestConditionError'); -- error
SELECT injection_points_detach('TestConditionError');
-- Attaching injection points that use the same name as one defined locally
-- previously should work.
SELECT injection_points_attach('TestConditionLocal1', 'error');
SELECT injection_points_detach('TestConditionLocal1');

DROP EXTENSION injection_points;
DROP FUNCTION wait_pid;
