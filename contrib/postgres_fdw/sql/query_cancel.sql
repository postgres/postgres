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
-- On most machines, 10ms will be enough to be sure that we've sent the slow
-- query.  We may sometimes exercise the race condition where we send cancel
-- before the remote side starts the query, but that's fine too.
SET LOCAL statement_timeout = '10ms';
-- This would take very long if not canceled:
SELECT count(*) FROM ft1 a CROSS JOIN ft1 b CROSS JOIN ft1 c CROSS JOIN ft1 d;
COMMIT;
