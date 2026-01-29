# PostgreSQL VACUUM Benchmark: ART vs QuART

This benchmark compares the performance of standard Adaptive Radix Tree (ART) vs QuART-optimized implementation in PostgreSQL's VACUUM operation.

Based on the pganalyze blog post: [5 mins of Postgres: Faster VACUUM with Adaptive Radix Trees](https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees)

## Quick Start

```bash
# Run benchmark with default settings (10M rows)
./benchmark_tidstore_vacuum.sh

# Run with custom parameters
./benchmark_tidstore_vacuum.sh -N 100000000 -m 64MB -r 5

# Analyze results
python3 analyze_benchmark.py
```

## Options

- `-N, --num-rows NUM`: Number of rows (default: 10,000,000)
- `-m, --memory SIZE`: maintenance_work_mem (default: 64MB)
- `-r, --runs NUM`: Runs per configuration (default: 3)
- `-h, --help`: Show help message

## What It Tests

1. **Builds PostgreSQL twice**: Once with standard ART, once with QuART
2. **Creates test table**: Specified number of rows with index
3. **Updates all rows**: Creates dead tuples for VACUUM to clean
4. **Runs VACUUM**: Measures time and index scan count
5. **Repeats**: Multiple runs for statistical significance

## Expected Results

Based on PostgreSQL 17 improvements:

| Rows | ART Time | QuART Time | Expected Improvement |
|------|----------|------------|---------------------|
| 10M  | ~80s     | ~60s       | ~25%                |
| 100M | ~770s    | ~620s      | ~20%                |

## Output Files

Results are saved in `benchmark_results/`:
- `benchmark_<rows>_<timestamp>.csv` - Raw data in CSV format
- `benchmark_plot_<timestamp>.png` - Visualizations (if Python analysis run)

### CSV Format

```
config,run,num_rows,maintenance_work_mem,index_vacuum_count,dead_tuple_bytes,elapsed_seconds,cpu_user,cpu_system
```

## Analysis

The Python script (`analyze_benchmark.py`) generates:

1. **Statistical Summary**: Mean, std deviation, min, max for all metrics
2. **Visualizations** (4 plots):
   - Elapsed time comparison (boxplot)
   - Index vacuum count (bar chart)
   - CPU time breakdown (stacked bar)
   - Performance improvement percentage
3. **Key Findings**: Consolidated improvement metrics

### Python Dependencies

```bash
pip3 install pandas matplotlib seaborn
```

## Implementation Details

The benchmark modifies `src/backend/access/common/tidstore.c` by setting `USE_QUART_TIDSTORE` at compile time:

- **ART Build**: `CFLAGS="-DUSE_QUART_TIDSTORE=0" ./configure ...`
- **QuART Build**: `CFLAGS="-DUSE_QUART_TIDSTORE=1" ./configure ...`

This flag controls which radix tree implementation is used in the TIDStore:
- `0`: Standard PostgreSQL Adaptive Radix Tree
- `1`: QuART-optimized radix tree

## Test Scenario

The benchmark simulates a realistic VACUUM workload:

```sql
-- Create table with N rows
CREATE TABLE vacuum_benchmark AS 
SELECT id, md5(id::text) as data
FROM generate_series(1, N) id;

-- Create index
CREATE INDEX vacuum_benchmark_idx ON vacuum_benchmark(id);

-- Update all rows (creates N dead tuples)
UPDATE vacuum_benchmark SET data = md5((id + 1)::text);

-- VACUUM with index cleanup
VACUUM (VERBOSE, INDEX_CLEANUP ON) vacuum_benchmark;
```

## Key Metrics

1. **Elapsed Time**: Total VACUUM duration (seconds)
2. **Index Vacuum Count**: Number of index scans during VACUUM
3. **CPU User Time**: User-space CPU time
4. **CPU System Time**: Kernel-space CPU time
5. **Dead Tuple Bytes**: Amount of dead tuple data processed

## System Requirements

- **Disk Space**: ~5GB for 10M row test, ~50GB for 100M rows
- **Memory**: Minimum 8GB RAM recommended
- **Time**: ~30 minutes per configuration (includes rebuild)
- **PostgreSQL**: Source code with QuART implementation

## Troubleshooting

### Build Failures

If rebuild fails, check:
```bash
./configure --prefix=$PWD/pg_install
make clean
make
```

### Database Won't Start

Check logs:
```bash
cat pg_data/logfile
tail -f pg_data/log/postgresql-*.log
```

### Missing Python Libraries

```bash
pip3 install pandas matplotlib seaborn
```

### Benchmark Hangs

Check if PostgreSQL is running:
```bash
pg_install/bin/pg_ctl -D pg_data status
```

Stop manually if needed:
```bash
pg_install/bin/pg_ctl -D pg_data stop -m fast
```

## Example Output

```
========================================
PostgreSQL VACUUM Benchmark
ART vs QuART Comparison
========================================

Parameters:
  Rows: 10000000
  Memory: 64MB
  Runs per config: 3

Testing Standard Radix Tree (ART)...
  Run 1/3...
    ✓ Completed: 78.234s, index_scans=2
  Run 2/3...
    ✓ Completed: 79.123s, index_scans=2
  Run 3/3...
    ✓ Completed: 77.891s, index_scans=2

Testing QuART-Optimized Radix Tree...
  Run 1/3...
    ✓ Completed: 58.456s, index_scans=1
  Run 2/3...
    ✓ Completed: 59.234s, index_scans=1
  Run 3/3...
    ✓ Completed: 58.901s, index_scans=1

========================================
Benchmark Complete!
========================================

Summary Statistics:

Configuration | Avg Elapsed (s) | Avg Index Scans | Improvement
------------- | --------------- | --------------- | -----------
ART           |           78.42 |             2.0 | baseline
QuART         |           58.86 |             1.0 | +24.9%
```

## Further Reading

- [PostgreSQL 17 Release Notes](https://www.postgresql.org/docs/17/release-17.html)
- [TIDStore Implementation](src/backend/access/common/tidstore.c)
- [Radix Tree Header](src/include/lib/radixtree.h)
- [QuART Paper](https://dl.acm.org/doi/10.1145/3183713.3196896)
