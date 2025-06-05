# TOAST Optimization Performance Testing Guide

This guide explains how to test the performance impact of the new `enable_toast_batch_optimization` feature in PostgreSQL 18.

## 🎯 What We're Testing

Our optimization improves TOAST (The Oversized-Attribute Storage Technique) performance by:

- **Batch fetching** multiple chunks in fewer IO operations
- **Reducing round-trips** to storage, especially beneficial for cloud/Aurora
- **Optimizing for larger values** (≥8KB threshold)

## 📋 Prerequisites

1. **PostgreSQL 18** with our patches applied and compiled
2. **pgcrypto extension** for generating test data
3. **Appropriate permissions** to create tables and run tests
4. **Clean test environment** for accurate measurements

## 🚀 Quick Start

### Step 1: Setup

```bash
# Make the benchmark script executable
chmod +x run_toast_benchmark.sh

# Ensure you're in the PostgreSQL source directory with the test files
ls toast_performance_test.sql run_toast_benchmark.sh
```

### Step 2: Run the Benchmark

```bash
# Run with default settings (localhost, postgres database)
./run_toast_benchmark.sh

# Or specify custom database connection
DB_NAME=mydb DB_USER=myuser DB_HOST=aurora.cluster.aws.com ./run_toast_benchmark.sh
```

### Step 3: Review Results

The benchmark creates a `toast_benchmark_results/` directory with:

- `test_before_optimization.out` - Results with optimization OFF
- `test_after_optimization.out` - Results with optimization ON
- `benchmark_summary.txt` - Comparison summary

## 📊 Key Metrics to Watch

### 1. **Execution Timing**

Look for timing improvements in:

```sql
-- Large data inserts (16KB+)
Time: 1250.123 ms  # Before
Time: 890.456 ms   # After (expect 20-40% improvement)

-- Full SELECT of large toast values
Time: 2100.789 ms  # Before
Time: 1456.234 ms  # After (expect 30-50% improvement)

-- Slice access (substring operations)
Time: 450.123 ms   # Before
Time: 280.456 ms   # After (expect 35-60% improvement)
```

### 2. **IO Statistics**

Critical metrics from `pg_statio_user_tables`:

```sql
-- Toast block reads (should decrease significantly)
toast_blks_read     | 15420  # Before
toast_blks_read     | 8750   # After (43% reduction)

-- Toast read time (should improve)
toast_blk_read_time | 2340.5 # Before (ms)
toast_blk_read_time | 1456.2 # After (38% improvement)

-- Hit ratios (may improve due to better caching)
toast_hit_ratio     | 67.3   # Before (%)
toast_hit_ratio     | 78.9   # After
```

### 3. **Buffer Usage**

From `EXPLAIN (ANALYZE, BUFFERS)`:

```
-- Look for reduced buffer reads in toast tables
Buffers: shared hit=1234 read=5678  # Before
Buffers: shared hit=2345 read=3456  # After
```

## 🔍 What to Look For

### ✅ **Expected Improvements**

- **20-50% faster** large toast value access
- **30-60% reduction** in toast block reads
- **Improved latency** on cloud storage (Aurora, GCP, Azure)
- **Better cache efficiency** due to batch operations

### ⚠️ **When Optimization Doesn't Help**

- Small data (< 8KB) - threshold not reached
- Single-chunk toast values - no batching benefit
- Memory-constrained systems - may use more memory
- Local SSD storage - less IO bottleneck to optimize

### 🚨 **Red Flags to Watch**

- **Increased memory usage** - monitor for OOM
- **Regression on small data** - should be minimal
- **Lock contention** - check for increased blocking

## 📈 Expected Performance Gains

Based on storage type:

| Storage Type            | Expected Improvement |
| ----------------------- | -------------------- |
| **Aurora/RDS**          | 40-60%               |
| **Cloud Block Storage** | 30-50%               |
| **Network Storage**     | 25-40%               |
| **Local SSD**           | 10-20%               |
| **Local HDD**           | 15-25%               |

## 🛠 Advanced Testing

### Test Different Thresholds

```sql
-- Test with different thresholds
SET enable_toast_batch_optimization = on;
-- Default threshold is 8KB, test various sizes:
-- 4KB, 8KB, 16KB, 32KB, 64KB, 128KB
```

### Test on Aurora Specifically

```bash
# Set up Aurora-specific tests
export DB_HOST=your-aurora-cluster.cluster-xyz.us-east-1.rds.amazonaws.com
export DB_NAME=performance_test
export DB_USER=performance_user

# Run extended tests
TEST_ROUNDS=5 ./run_toast_benchmark.sh
```

### Monitor System Resources

```bash
# While tests run, monitor in another terminal:
iostat -x 1   # IO utilization
top -p $(pgrep postgres)  # Memory and CPU usage

# PostgreSQL-specific monitoring:
psql -c "SELECT * FROM pg_stat_activity WHERE query ~ 'toast_perf_test';"
```

## 📋 Test Configuration Details

The test script creates data with these characteristics:

| Data Type     | Size | Toast Behavior              | Optimization  |
| ------------- | ---- | --------------------------- | ------------- |
| `small_data`  | 1KB  | No TOAST                    | N/A           |
| `medium_data` | 4KB  | TOAST, below threshold      | Traditional   |
| `large_data`  | 16KB | TOAST, above threshold      | **Optimized** |
| `huge_data`   | 64KB | TOAST, well above threshold | **Optimized** |

## 🐛 Troubleshooting

### Common Issues

1. **Permission Denied**

   ```bash
   chmod +x run_toast_benchmark.sh
   ```

2. **pgcrypto Extension Missing**

   ```sql
   CREATE EXTENSION pgcrypto;
   ```

3. **Connection Failed**

   ```bash
   # Check connection parameters
   psql -h $DB_HOST -p $DB_PORT -U $DB_USER -d $DB_NAME -c "SELECT 1;"
   ```

4. **Out of Memory**
   ```sql
   -- Reduce test data size
   -- Edit toast_performance_test.sql and reduce generate_series() counts
   ```

### Verification Commands

```sql
-- Verify GUC is available
SHOW enable_toast_batch_optimization;

-- Check toast table exists and has data
SELECT schemaname, tablename, n_tup_ins, n_tup_upd
FROM pg_stat_user_tables
WHERE tablename LIKE '%toast%';

-- Verify data sizes
SELECT
    pg_size_pretty(pg_total_relation_size('toast_perf_test')) as total_size,
    pg_size_pretty(pg_total_relation_size('toast_perf_test') -
                   pg_relation_size('toast_perf_test')) as toast_size;
```

## 📊 Interpreting Results

### Success Indicators

- ✅ Reduced execution times for large data operations
- ✅ Fewer `toast_blks_read` in statistics
- ✅ Improved `toast_blk_read_time`
- ✅ Better performance on cloud storage

### Acceptable Results

- 🟡 No regression on small data
- 🟡 Minimal memory increase
- 🟡 Stable performance on traditional storage

### Investigation Needed

- ❌ Slower performance on optimized operations
- ❌ Significantly increased memory usage
- ❌ System instability or crashes

## 🎯 Real-World Validation

For production validation:

1. **Test with your actual workload** - use real data patterns
2. **Monitor during peak hours** - check for any regressions
3. **Gradual rollout** - enable on non-critical systems first
4. **Watch key metrics** - response time, IO usage, memory

## 📞 Support

If you encounter issues or unexpected results:

1. Check the troubleshooting section above
2. Review PostgreSQL logs for any errors
3. Compare your results with the expected ranges
4. Consider your specific storage and workload characteristics

Remember: The optimization is most beneficial for **large toast values** on **cloud/network storage** systems like Aurora.
