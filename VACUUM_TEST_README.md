# PostgreSQL 17 VACUUM Adaptive Radix Tree Test

This test demonstrates the performance improvements in PostgreSQL 17's VACUUM operation using adaptive radix trees, as described in the [pganalyze blog post](https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees).

## Background

PostgreSQL 17 introduces a significant improvement to VACUUM performance through the use of **adaptive radix trees** for storing dead tuple identifiers (TIDs). This improvement:

- **Reduces memory consumption** - More efficient storage of dead tuple information
- **Reduces index vacuum phases** - Fewer round trips to vacuum indexes
- **Improves overall VACUUM speed** - Especially for tables with many dead tuples

### Key Differences: PG16 vs PG17

**PostgreSQL 16 and earlier:**
- Uses a simple array to store dead tuple TIDs
- Limited to ~11M tuples per 64MB of `maintenance_work_mem`
- Requires multiple index vacuum phases when memory fills up
- Each index vacuum phase adds significant overhead

**PostgreSQL 17:**
- Uses adaptive radix trees (based on the [ART paper](https://db.in.tum.de/~leis/papers/ART.pdf))
- ~2x more memory efficient
- Can store more dead tuples in the same memory footprint
- Significantly fewer index vacuum phases
- 20-25% faster VACUUM operations on large tables

## Files

- **`test_vacuum_radix_tree.sql`** - Main test script that creates a table, updates it, and runs VACUUM
- **`test_vacuum_monitor.sql`** - Monitoring script to watch VACUUM progress in real-time (run in separate session)
- **`run_vacuum_test.sh`** - Shell script to automate the test execution

## Prerequisites

1. PostgreSQL installation (preferably PG17 to see improvements, but works on PG16+ for comparison)
2. `psql` command-line tool
3. Database with sufficient space (~500MB for 10M row test, ~5GB for 100M row test)
4. Permissions to create tables and run VACUUM

## Running the Test

### Quick Start

```bash
# Make sure you're in the QuART-VACUUM directory
cd /Users/cangokmen/Desktop/QuART-VACUUM

# Run the automated test script
./run_vacuum_test.sh
```

### Manual Execution

```bash
# Run the main test
psql -d postgres -f test_vacuum_radix_tree.sql

# (Optional) In a separate terminal, monitor progress
psql -d postgres -f test_vacuum_monitor.sql
```

### Customizing the Test

You can modify the test parameters in `test_vacuum_radix_tree.sql`:

```sql
-- Change the number of rows (10M is default, 100M for full test)
CREATE TABLE test_vacuum_art AS 
SELECT * FROM generate_series(1, 100000000) x(id);  -- Change here

-- Adjust memory settings
SET maintenance_work_mem = '64MB';  -- Lower = more index vacuum phases
```

## What to Look For

### In PostgreSQL 16 and Earlier

```
automatic vacuum of table "postgres.public.test_vacuum_art": index scans: 9
	pages: 0 removed, 884956 remain, 884956 scanned (100.00% of total)
	...
	system usage: CPU: user: 78.21 s, system: 30.06 s, elapsed: 773.23 s
```

Key observations:
- **9 index scans** - Multiple round trips to vacuum the index
- **max_dead_tuples**: ~11M tuples per 64MB
- **Longer elapsed time**

### In PostgreSQL 17

```
automatic vacuum of table "postgres.public.test_vacuum_art": index scans: 1
	pages: 0 removed, 884956 remain, 884956 scanned (100.00% of total)
	...
	system usage: CPU: user: 78.84 s, system: 47.42 s, elapsed: 619.04 s
```

Key observations:
- **1 index scan** - Single round trip to vacuum the index
- **dead_tuple_bytes**: ~37MB to store 100M dead tuples (in 64MB memory)
- **~20% faster elapsed time**

### Monitoring with `pg_stat_progress_vacuum`

The `test_vacuum_monitor.sql` script shows:

```
phase                | vacuuming indexes
index_vacuum_count   | 0
max_dead_tuple_bytes | 67108864  (PG17 - in bytes)
dead_tuple_bytes     | 37289984  (PG17 - current usage)
max_dead_tuples      | 11184809  (PG16 - tuple count)
num_dead_tuples      | 10522080  (PG16 - current count)
```

## Understanding the Radix Tree Logging

With the logging we added to the radix tree implementation, you'll see:

```
LOG:  Radix tree: inserted new key 12345
LOG:  Radix tree: updated existing key 12345
```

This shows which TIDs (tuple identifiers) are being stored in the radix tree during VACUUM.

## Benchmarking Tips

For meaningful benchmarks:

1. **Test with different table sizes**: 1M, 10M, 100M rows
2. **Vary memory settings**: Try 64MB, 128MB, 256MB `maintenance_work_mem`
3. **Test with multiple indexes**: Add 2-3 indexes to see compounding benefits
4. **Measure index vacuum phases**: Count `index_vacuum_count` in results
5. **Track elapsed time**: Compare total VACUUM duration

## Cleanup

To remove the test table:

```sql
DROP TABLE IF EXISTS test_vacuum_art CASCADE;
```

Or use:

```bash
psql -d postgres -c "DROP TABLE IF EXISTS test_vacuum_art CASCADE;"
```

## References

- [pganalyze Blog Post](https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees)
- [Adaptive Radix Tree Commit](https://git.postgresql.org/gitweb/?p=postgresql.git;a=commit;h=ee1b30f128d8f63a5184d2bcf1c48a3efc3fcbf9)
- [TIDStore Commit](https://git.postgresql.org/gitweb/?p=postgresql.git;a=commit;h=30e144287a)
- [VACUUM with TIDStore Commit](https://git.postgresql.org/gitweb/?p=postgresql.git;a=commitdiff;h=667e65aac354975c6f8090c6146fceb8d7b762d6)
- [ART Paper (2013)](https://db.in.tum.de/~leis/papers/ART.pdf)

## Expected Results

**Small test (10M rows, 64MB maintenance_work_mem):**
- PG16: 2-3 index vacuum phases, ~80-100 seconds
- PG17: 1 index vacuum phase, ~60-80 seconds

**Full test (100M rows, 64MB maintenance_work_mem):**
- PG16: 9 index vacuum phases, ~770 seconds
- PG17: 1 index vacuum phase, ~620 seconds (~20% improvement)

## Troubleshooting

**"permission denied" error:**
```bash
chmod +x run_vacuum_test.sh
```

**"relation does not exist" error:**
The test will create the table automatically.

**Out of memory:**
Reduce the number of rows in the test or increase `maintenance_work_mem`.

**Can't see logs:**
Enable vacuum logging in postgresql.conf:
```
log_autovacuum_min_duration = 0
```

## Additional Notes

The radix tree implementation is used not only for VACUUM but also for:
- TID bitmap scans
- Index-only scans with many matching rows
- Other operations requiring efficient storage of large sets of TIDs

This is a foundational improvement that benefits many PostgreSQL operations beyond just VACUUM.
