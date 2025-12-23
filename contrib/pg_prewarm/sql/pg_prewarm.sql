-- Test pg_prewarm extension
CREATE EXTENSION pg_prewarm;

-- pg_prewarm() should fail if the target relation has no storage.
CREATE TABLE test (c1 int) PARTITION BY RANGE (c1);
SELECT pg_prewarm('test', 'buffer');

-- Cleanup
DROP TABLE test;
DROP EXTENSION pg_prewarm;
