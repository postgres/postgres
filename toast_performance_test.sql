-- =====================================================
-- PostgreSQL TOAST Optimization Performance Test
-- =====================================================
-- This script tests the impact of enable_toast_batch_optimization
-- with LARGE data sizes and comprehensive metrics

\timing on
\set ECHO all

-- Enable comprehensive timing and statistics
SET track_io_timing = ON;
SET track_functions = 'all';
SET log_min_duration_statement = 0;

-- Create extension for generating random data if available
CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- =====================================================
-- SETUP: Create test tables with larger data
-- =====================================================

DROP TABLE IF EXISTS toast_perf_test CASCADE;
CREATE TABLE toast_perf_test (
    id SERIAL PRIMARY KEY,
    test_type VARCHAR(20),
    small_data TEXT,      -- ~2KB (no toast)
    medium_data TEXT,     -- ~8KB (at threshold)
    large_data TEXT,      -- ~64KB (well above threshold)
    huge_data TEXT,       -- ~256KB (very large)
    massive_data TEXT,    -- ~1MB (massive)
    created_at TIMESTAMP DEFAULT now()
);

-- Create index for testing slice access patterns
CREATE INDEX idx_toast_perf_test_id ON toast_perf_test(id);
CREATE INDEX idx_toast_perf_test_type ON toast_perf_test(test_type);

-- =====================================================
-- ENHANCED DATA GENERATION
-- =====================================================

-- Function to generate test data of specific sizes (more accurate sizing)
CREATE OR REPLACE FUNCTION generate_test_data(target_kb integer)
RETURNS TEXT AS $$
DECLARE
    base_string TEXT;
    result TEXT := '';
    iterations INTEGER;
BEGIN
    -- Create a 1KB base string (1024 characters)
    base_string := repeat('A', 512) || repeat('B', 256) || repeat('C', 128) || repeat('D', 64) || repeat('E', 32) || repeat('F', 16) || repeat('G', 8) || repeat('H', 4) || repeat('I', 2) || repeat('J', 2);

    -- Calculate iterations needed
    iterations := target_kb;

    -- Build the target size string
    FOR i IN 1..iterations LOOP
        result := result || base_string;
    END LOOP;

    RETURN result;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- COMPREHENSIVE BASELINE METRICS
-- =====================================================

-- Reset all statistics for clean measurement
SELECT pg_stat_reset();
SELECT pg_stat_reset_shared('bgwriter');
SELECT pg_stat_reset_shared('archiver');

-- Check current GUC setting
SHOW enable_toast_batch_optimization;

-- Show current buffer and cache settings
SELECT
    name,
    setting,
    unit,
    context
FROM pg_settings
WHERE name IN ('shared_buffers', 'effective_cache_size', 'work_mem', 'maintenance_work_mem');

-- Record start time and show system info
\echo '====== STARTING COMPREHENSIVE PERFORMANCE TEST ======'
SELECT
    now() as test_start_time,
    pg_size_pretty(pg_database_size(current_database())) as db_size_before;

-- =====================================================
-- TEST 1: INSERT Performance (Toast Creation) - LARGE DATA
-- =====================================================

\echo '====== TEST 1: INSERT Performance with LARGE Data ======'

-- Small baseline (2KB - no toast)
\echo 'Inserting 200 small records (2KB each = 400KB total)...'
\timing on
INSERT INTO toast_perf_test (test_type, small_data)
SELECT 'small', generate_test_data(2) FROM generate_series(1, 200);
\timing off

-- Medium data (8KB - at threshold)
\echo 'Inserting 100 medium records (8KB each = 800KB total)...'
\timing on
INSERT INTO toast_perf_test (test_type, medium_data)
SELECT 'medium', generate_test_data(8) FROM generate_series(1, 100);
\timing off

-- Large data (64KB - well above threshold)
\echo 'Inserting 100 large records (64KB each = 6.4MB total)...'
\timing on
INSERT INTO toast_perf_test (test_type, large_data)
SELECT 'large', generate_test_data(64) FROM generate_series(1, 100);
\timing off

-- Huge data (256KB - very large)
\echo 'Inserting 50 huge records (256KB each = 12.8MB total)...'
\timing on
INSERT INTO toast_perf_test (test_type, huge_data)
SELECT 'huge', generate_test_data(256) FROM generate_series(1, 50);
\timing off

-- Massive data (1MB - massive size)
\echo 'Inserting 20 massive records (1MB each = 20MB total)...'
\timing on
INSERT INTO toast_perf_test (test_type, massive_data)
SELECT 'massive', generate_test_data(1024) FROM generate_series(1, 20);
\timing off

-- Show intermediate statistics
\echo '====== POST-INSERT STATISTICS ======'
SELECT
    schemaname,
    tablename,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) as total_size,
    pg_size_pretty(pg_relation_size(schemaname||'.'||tablename)) as table_size,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename) - pg_relation_size(schemaname||'.'||tablename)) as toast_size,
    CASE
        WHEN pg_relation_size(schemaname||'.'||tablename) > 0
        THEN round(((pg_total_relation_size(schemaname||'.'||tablename) - pg_relation_size(schemaname||'.'||tablename))::numeric / pg_total_relation_size(schemaname||'.'||tablename)) * 100, 1)
        ELSE 0
    END as toast_percentage
FROM pg_tables
WHERE tablename = 'toast_perf_test';

-- =====================================================
-- TEST 2: READ Performance (Complete Detoasting) - COMPREHENSIVE
-- =====================================================

\echo '====== TEST 2: READ Performance - Full Detoasting ======'

-- Force buffer eviction for more realistic testing
CHECKPOINT;

\echo 'Reading all medium records (8KB each - threshold test)...'
\timing on
SELECT id, test_type, length(medium_data), substr(medium_data, 1, 100), substr(medium_data, 4000, 100)
FROM toast_perf_test
WHERE medium_data IS NOT NULL
ORDER BY id;
\timing off

\echo 'Reading all large records (64KB each - primary optimization target)...'
\timing on
SELECT id, test_type, length(large_data), substr(large_data, 1, 100), substr(large_data, 32000, 100)
FROM toast_perf_test
WHERE large_data IS NOT NULL
ORDER BY id;
\timing off

\echo 'Reading all huge records (256KB each - heavy optimization)...'
\timing on
SELECT id, test_type, length(huge_data), substr(huge_data, 1, 100), substr(huge_data, 128000, 100)
FROM toast_perf_test
WHERE huge_data IS NOT NULL
ORDER BY id;
\timing off

\echo 'Reading all massive records (1MB each - maximum optimization)...'
\timing on
SELECT id, test_type, length(massive_data), substr(massive_data, 1, 100), substr(massive_data, 512000, 100)
FROM toast_perf_test
WHERE massive_data IS NOT NULL
ORDER BY id;
\timing off

-- =====================================================
-- TEST 3: SLICE ACCESS Performance (Partial Detoasting) - COMPREHENSIVE
-- =====================================================

\echo '====== TEST 3: SLICE ACCESS Performance ======'

\echo 'Testing multiple slice access on large data (64KB)...'
\timing on
SELECT
    id,
    substr(large_data, 1, 2000) as slice1,
    substr(large_data, 16000, 2000) as slice2,
    substr(large_data, 32000, 2000) as slice3,
    substr(large_data, 48000, 2000) as slice4
FROM toast_perf_test
WHERE large_data IS NOT NULL
ORDER BY id
LIMIT 50;
\timing off

\echo 'Testing multiple slice access on huge data (256KB)...'
\timing on
SELECT
    id,
    substr(huge_data, 1, 4000) as slice1,
    substr(huge_data, 64000, 4000) as slice2,
    substr(huge_data, 128000, 4000) as slice3,
    substr(huge_data, 192000, 4000) as slice4
FROM toast_perf_test
WHERE huge_data IS NOT NULL
ORDER BY id
LIMIT 25;
\timing off

\echo 'Testing slice access on massive data (1MB)...'
\timing on
SELECT
    id,
    substr(massive_data, 1, 8000) as slice1,
    substr(massive_data, 256000, 8000) as slice2,
    substr(massive_data, 512000, 8000) as slice3,
    substr(massive_data, 768000, 8000) as slice4
FROM toast_perf_test
WHERE massive_data IS NOT NULL
ORDER BY id
LIMIT 10;
\timing off

-- =====================================================
-- TEST 4: REPEATED ACCESS Performance - CACHE EFFECTS
-- =====================================================

\echo '====== TEST 4: REPEATED ACCESS Performance ======'

\echo 'First access to large toast values (cold cache)...'
\timing on
SELECT COUNT(*), AVG(length(large_data)), MIN(length(large_data)), MAX(length(large_data))
FROM toast_perf_test
WHERE large_data IS NOT NULL;
\timing off

\echo 'Second access to same large toast values (warm cache)...'
\timing on
SELECT COUNT(*), AVG(length(large_data)), MIN(length(large_data)), MAX(length(large_data))
FROM toast_perf_test
WHERE large_data IS NOT NULL;
\timing off

\echo 'Third access to same large toast values (hot cache)...'
\timing on
SELECT COUNT(*), AVG(length(large_data)), MIN(length(large_data)), MAX(length(large_data))
FROM toast_perf_test
WHERE large_data IS NOT NULL;
\timing off

-- Test with massive data
\echo 'Repeated access to massive toast values (1MB each)...'
\timing on
SELECT COUNT(*), AVG(length(massive_data))::bigint as avg_size_bytes
FROM toast_perf_test
WHERE massive_data IS NOT NULL;
\timing off

\timing on
SELECT COUNT(*), AVG(length(massive_data))::bigint as avg_size_bytes
FROM toast_perf_test
WHERE massive_data IS NOT NULL;
\timing off

-- =====================================================
-- TEST 5: WRITE Performance - UPDATES
-- =====================================================

\echo '====== TEST 5: UPDATE Performance ======'

\echo 'Updating large data (testing write performance)...'
\timing on
UPDATE toast_perf_test
SET large_data = generate_test_data(64)
WHERE test_type = 'large' AND id <= 10;
\timing off

\echo 'Updating massive data (testing large write performance)...'
\timing on
UPDATE toast_perf_test
SET massive_data = generate_test_data(1024)
WHERE test_type = 'massive' AND id <= 5;
\timing off

-- =====================================================
-- COMPREHENSIVE PERFORMANCE METRICS COLLECTION
-- =====================================================

\echo '====== COMPREHENSIVE PERFORMANCE METRICS ======'

-- Detailed table and toast sizes
SELECT
    schemaname,
    tablename,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) as total_size,
    pg_size_pretty(pg_relation_size(schemaname||'.'||tablename)) as heap_size,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename) - pg_relation_size(schemaname||'.'||tablename)) as toast_index_size,
    n_tup_ins as rows_inserted,
    n_tup_upd as rows_updated,
    n_tup_del as rows_deleted
FROM pg_tables t
JOIN pg_stat_user_tables s ON t.tablename = s.relname
WHERE tablename = 'toast_perf_test';

-- Detailed IO Statistics with calculations
SELECT
    schemaname,
    relname,
    seq_scan,
    seq_tup_read,
    idx_scan,
    idx_tup_fetch,
    n_tup_ins,
    n_tup_upd,
    n_tup_del,
    heap_blks_read,
    heap_blks_hit,
    toast_blks_read,
    toast_blks_hit,
    CASE
        WHEN heap_blks_read + heap_blks_hit > 0
        THEN round((heap_blks_hit::numeric / (heap_blks_read + heap_blks_hit)) * 100, 2)
        ELSE 0
    END as heap_hit_ratio,
    CASE
        WHEN toast_blks_read + toast_blks_hit > 0
        THEN round((toast_blks_hit::numeric / (toast_blks_read + toast_blks_hit)) * 100, 2)
        ELSE 0
    END as toast_hit_ratio,
    heap_blks_read + toast_blks_read as total_reads,
    heap_blks_hit + toast_blks_hit as total_hits
FROM pg_statio_user_tables
WHERE relname = 'toast_perf_test';

-- Show buffer cache effectiveness
SELECT
    name,
    setting,
    unit,
    short_desc
FROM pg_settings
WHERE name IN ('shared_buffers', 'effective_cache_size', 'enable_toast_batch_optimization', 'work_mem');

-- Row count by test type for verification
SELECT
    test_type,
    COUNT(*) as record_count,
    pg_size_pretty(AVG(CASE
        WHEN small_data IS NOT NULL THEN length(small_data)
        WHEN medium_data IS NOT NULL THEN length(medium_data)
        WHEN large_data IS NOT NULL THEN length(large_data)
        WHEN huge_data IS NOT NULL THEN length(huge_data)
        WHEN massive_data IS NOT NULL THEN length(massive_data)
        ELSE 0
    END)::bigint) as avg_data_size
FROM toast_perf_test
GROUP BY test_type
ORDER BY test_type;

-- Database size after test
SELECT
    pg_size_pretty(pg_database_size(current_database())) as db_size_after,
    pg_size_pretty(pg_total_relation_size('toast_perf_test')) as test_table_size;

\echo '====== TEST COMPLETED ======'
SELECT
    now() as test_end_time,
    extract(epoch from (now() - (SELECT min(created_at) FROM toast_perf_test))) as total_test_duration_seconds;

-- =====================================================
-- EXECUTION PLANS FOR ANALYSIS
-- =====================================================

\echo '====== EXECUTION PLANS FOR OPTIMIZATION ANALYSIS ======'

-- Plan for large data access
EXPLAIN (ANALYZE, BUFFERS, VERBOSE)
SELECT id, length(large_data), substr(large_data, 1, 1000)
FROM toast_perf_test
WHERE large_data IS NOT NULL
LIMIT 10;

-- Plan for slice access
EXPLAIN (ANALYZE, BUFFERS, VERBOSE)
SELECT id, substr(massive_data, 1, 4000), substr(massive_data, 512000, 4000)
FROM toast_perf_test
WHERE massive_data IS NOT NULL
LIMIT 5;
