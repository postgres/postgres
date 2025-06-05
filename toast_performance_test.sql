-- =====================================================
-- PostgreSQL TOAST Optimization Performance Test
-- =====================================================
-- This script tests the impact of enable_toast_batch_optimization
-- Run this before and after applying the optimization patches

\timing on
\set ECHO all

-- Enable additional timing and statistics
SET track_io_timing = ON;
SET log_min_duration_statement = 0;  -- Log all statements for timing analysis

-- Create extension for generating random data if available
CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- =====================================================
-- SETUP: Create test tables
-- =====================================================

DROP TABLE IF EXISTS toast_perf_test;
CREATE TABLE toast_perf_test (
    id SERIAL PRIMARY KEY,
    small_data TEXT,
    medium_data TEXT,   -- ~4KB (below threshold)
    large_data TEXT,    -- ~16KB (above threshold)
    huge_data TEXT,     -- ~64KB (well above threshold)
    created_at TIMESTAMP DEFAULT now()
);

-- Create index for testing slice access patterns
CREATE INDEX idx_toast_perf_test_id ON toast_perf_test(id);

-- =====================================================
-- TEST DATA GENERATION
-- =====================================================

-- Function to generate test data of specific sizes
CREATE OR REPLACE FUNCTION generate_test_data(size_kb integer)
RETURNS TEXT AS $$
BEGIN
    -- Generate random string of approximately size_kb kilobytes
    RETURN repeat(encode(gen_random_bytes(32), 'hex'), size_kb * 16);
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- BASELINE METRICS COLLECTION
-- =====================================================

-- Reset statistics
SELECT pg_stat_reset();
SELECT pg_stat_reset_shared('bgwriter');

-- Check current GUC setting
SHOW enable_toast_batch_optimization;

-- Record start time
\echo '====== STARTING PERFORMANCE TEST ======'
SELECT now() as test_start_time;

-- =====================================================
-- TEST 1: INSERT Performance (Toast Creation)
-- =====================================================

\echo '====== TEST 1: INSERT Performance ======'

-- Small baseline (no toast)
\echo 'Inserting 100 small records (1KB each)...'
\timing on
INSERT INTO toast_perf_test (small_data)
SELECT generate_test_data(1) FROM generate_series(1, 100);
\timing off

-- Medium data (below optimization threshold)
\echo 'Inserting 50 medium records (4KB each)...'
\timing on
INSERT INTO toast_perf_test (medium_data)
SELECT generate_test_data(4) FROM generate_series(1, 50);
\timing off

-- Large data (above optimization threshold)
\echo 'Inserting 50 large records (16KB each)...'
\timing on
INSERT INTO toast_perf_test (large_data)
SELECT generate_test_data(16) FROM generate_series(1, 50);
\timing off

-- Huge data (well above threshold)
\echo 'Inserting 20 huge records (64KB each)...'
\timing on
INSERT INTO toast_perf_test (huge_data)
SELECT generate_test_data(64) FROM generate_series(1, 20);
\timing off

-- =====================================================
-- TEST 2: FULL SELECT Performance (Complete Detoasting)
-- =====================================================

\echo '====== TEST 2: FULL SELECT Performance ======'

-- Clear buffer cache to simulate cold reads
SELECT pg_prewarm('toast_perf_test', 'buffer');

\echo 'Selecting all large records (full detoast)...'
\timing on
SELECT id, length(large_data), substr(large_data, 1, 100)
FROM toast_perf_test
WHERE large_data IS NOT NULL
ORDER BY id;
\timing off

\echo 'Selecting all huge records (full detoast)...'
\timing on
SELECT id, length(huge_data), substr(huge_data, 1, 100)
FROM toast_perf_test
WHERE huge_data IS NOT NULL
ORDER BY id;
\timing off

-- =====================================================
-- TEST 3: SLICE ACCESS Performance (Partial Detoasting)
-- =====================================================

\echo '====== TEST 3: SLICE ACCESS Performance ======'

\echo 'Testing substring access (slice access) on large data...'
\timing on
SELECT id, substr(large_data, 1, 1000), substr(large_data, 5000, 1000)
FROM toast_perf_test
WHERE large_data IS NOT NULL
ORDER BY id
LIMIT 25;
\timing off

\echo 'Testing substring access on huge data...'
\timing on
SELECT id, substr(huge_data, 1, 2000), substr(huge_data, 30000, 2000)
FROM toast_perf_test
WHERE huge_data IS NOT NULL
ORDER BY id
LIMIT 10;
\timing off

-- =====================================================
-- TEST 4: REPEATED ACCESS Performance
-- =====================================================

\echo '====== TEST 4: REPEATED ACCESS Performance ======'

\echo 'Repeated access to same large toast values...'
\timing on
SELECT COUNT(*), AVG(length(large_data))
FROM toast_perf_test
WHERE large_data IS NOT NULL;
\timing off

\timing on
SELECT COUNT(*), AVG(length(large_data))
FROM toast_perf_test
WHERE large_data IS NOT NULL;
\timing off

\timing on
SELECT COUNT(*), AVG(length(large_data))
FROM toast_perf_test
WHERE large_data IS NOT NULL;
\timing off

-- =====================================================
-- PERFORMANCE METRICS COLLECTION
-- =====================================================

\echo '====== PERFORMANCE METRICS ======'

-- Table sizes and toast usage
SELECT
    schemaname,
    tablename,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) as total_size,
    pg_size_pretty(pg_relation_size(schemaname||'.'||tablename)) as table_size,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename) - pg_relation_size(schemaname||'.'||tablename)) as toast_size
FROM pg_tables
WHERE tablename = 'toast_perf_test';

-- IO Statistics
SELECT
    schemaname,
    relname,
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
    END as toast_hit_ratio
FROM pg_statio_user_tables
WHERE relname = 'toast_perf_test';

-- Buffer cache statistics
SELECT
    name,
    setting,
    unit
FROM pg_settings
WHERE name IN ('shared_buffers', 'effective_cache_size', 'enable_toast_batch_optimization');

-- Timing statistics (if track_io_timing enabled)
SELECT
    schemaname,
    relname,
    heap_blk_read_time,
    heap_blk_write_time,
    toast_blk_read_time,
    toast_blk_write_time
FROM pg_statio_user_tables
WHERE relname = 'toast_perf_test';

\echo '====== TEST COMPLETED ======'
SELECT now() as test_end_time;

-- Optional: Show query execution plans for analysis
\echo '====== EXECUTION PLANS ======'
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, length(large_data)
FROM toast_perf_test
WHERE large_data IS NOT NULL
LIMIT 5;
