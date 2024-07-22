SELECT version() ~ 'cygwin' AS skip_test \gset
\if :skip_test
\quit
\endif

-- Make sure this big CROSS JOIN query is pushed down
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM ft1 CROSS JOIN ft2 CROSS JOIN ft4 CROSS JOIN ft5;
-- Make sure query cancellation works
BEGIN;
SET LOCAL statement_timeout = '10ms';
select count(*) from ft1 CROSS JOIN ft2 CROSS JOIN ft4 CROSS JOIN ft5; -- this takes very long
COMMIT;
