-- Test script to demonstrate Postgres 17's adaptive radix tree in VACUUM
-- Based on the pganalyze blog post example
-- https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees

-- Enable detailed logging for vacuum operations
SET client_min_messages = 'LOG';
SET log_autovacuum_min_duration = 0;

-- Set maintenance_work_mem to a low value to trigger multiple index vacuum phases
-- in older versions, but fewer in Postgres 17 with adaptive radix trees
SET maintenance_work_mem = '64MB';

-- Show current settings
SHOW maintenance_work_mem;
SHOW autovacuum_work_mem;

-- Drop the test table if it exists
DROP TABLE IF EXISTS test_vacuum_art CASCADE;

-- Create a test table with 10 million records (reduced from 100M for faster testing)
-- You can increase this to 100M for a full test like in the blog post
SELECT 'Creating test table with 10 million rows...' AS status;
CREATE TABLE test_vacuum_art AS 
SELECT * FROM generate_series(1, 10000000) x(id);

-- Create an index - this is critical because the improvement only matters
-- on tables with indexes
SELECT 'Creating index...' AS status;
CREATE INDEX test_vacuum_art_idx ON test_vacuum_art(id);

-- Check table size
SELECT 
    pg_size_pretty(pg_total_relation_size('test_vacuum_art')) AS table_size,
    pg_size_pretty(pg_relation_size('test_vacuum_art')) AS heap_size,
    pg_size_pretty(pg_indexes_size('test_vacuum_art')) AS index_size;

-- Update the whole table to create dead tuples
-- This will cause VACUUM to need to track all the old row versions
SELECT 'Updating all rows to create dead tuples...' AS status;
UPDATE test_vacuum_art SET id = id - 1;

-- Check pg_stat_all_tables to see dead tuples
SELECT 
    n_live_tup,
    n_dead_tup,
    n_mod_since_analyze
FROM pg_stat_all_tables 
WHERE relname = 'test_vacuum_art';

-- Now run VACUUM manually with VERBOSE to see the detailed output
-- In Postgres 17 with adaptive radix trees, you should see:
-- - More efficient memory usage (dead_tuple_bytes instead of just counting tuples)
-- - Fewer index vacuum phases
-- - Overall faster vacuum completion time
SELECT 'Running VACUUM VERBOSE...' AS status;
VACUUM (VERBOSE, INDEX_CLEANUP ON) test_vacuum_art;

-- Query pg_stat_progress_vacuum during vacuum (if run in another session)
-- You can run this in a separate psql session while VACUUM is running:
-- SELECT * FROM pg_stat_progress_vacuum;

-- Check final statistics
SELECT 
    n_live_tup,
    n_dead_tup,
    n_mod_since_analyze,
    last_vacuum,
    vacuum_count
FROM pg_stat_all_tables 
WHERE relname = 'test_vacuum_art';

-- Cleanup
-- DROP TABLE test_vacuum_art;

SELECT 'Test completed! Check the VACUUM output above for:' AS summary;
SELECT '- index_vacuum_count: should be lower in PG17' AS metric_1;
SELECT '- dead_tuple_bytes: new in PG17 (instead of num_dead_tuples)' AS metric_2;
SELECT '- elapsed time: should be faster in PG17' AS metric_3;
