SELECT version() ~ 'cygwin' AS skip_test \gset
\if :skip_test
\quit
\endif

-- Let's test canceling a remote query.  Use a table that does not have
-- remote_estimate enabled, else there will be multiple queries to the
-- remote and we might unluckily send the cancel in between two of them.
-- First let's confirm that the query is actually pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM ft1 a CROSS JOIN ft1 b CROSS JOIN ft1 c CROSS JOIN ft1 d;

BEGIN;
-- Make sure that connection is open and set up.
SELECT count(*) FROM ft1 a;
-- Timeout needs to be long enough to be sure that we've sent the slow query.
SET LOCAL statement_timeout = '100ms';
-- This would take very long if not canceled:
SELECT count(*) FROM ft1 a CROSS JOIN ft1 b CROSS JOIN ft1 c CROSS JOIN ft1 d;
COMMIT;
