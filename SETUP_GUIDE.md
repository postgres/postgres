# PostgreSQL 17 VACUUM Adaptive Radix Tree - Complete Testing Guide

This guide will help you build PostgreSQL from source, configure it, and run tests to demonstrate the VACUUM performance improvements using adaptive radix trees.

## 🎯 What You'll Test

Based on the [pganalyze blog post](https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees), this test demonstrates:

- **Postgres 16**: 9 index vacuum phases, ~770 seconds (for 100M rows)
- **Postgres 17**: 1 index vacuum phase, ~620 seconds (~20% faster)
- **Memory efficiency**: 2x better storage of dead tuple identifiers

## 📋 Prerequisites

Before starting, ensure you have:

- macOS with Xcode Command Line Tools
- At least 5GB free disk space
- About 30 minutes for initial build

To install Xcode Command Line Tools:
```bash
xcode-select --install
```

## 🚀 Quick Start

### 1. Build PostgreSQL

Run the automated build script:

```bash
./build_and_setup.sh
```

This will:
- Configure PostgreSQL with debugging enabled
- Compile the source code (5-10 minutes)
- Install to `pg_install/` directory
- Initialize a database cluster in `pg_data/`
- Create helper scripts for starting/stopping PostgreSQL

### 2. Start PostgreSQL

```bash
./start_postgres.sh
```

### 3. Run the VACUUM Test

```bash
./run_vacuum_test.sh
```

This will:
- Create a 10M row table
- Create an index
- Update all rows (creating dead tuples)
- Run VACUUM and show detailed statistics

### 4. Monitor Vacuum Progress (Optional)

In a separate terminal:

```bash
./pg_install/bin/psql -d postgres -f test_vacuum_monitor.sql
```

This will show real-time vacuum progress every 2 seconds.

### 5. Stop PostgreSQL

```bash
./stop_postgres.sh
```

## 📁 Files Created

After running `build_and_setup.sh`, you'll have:

```
QuART-VACUUM/
├── build_and_setup.sh           # Main build script
├── start_postgres.sh            # Start PostgreSQL
├── stop_postgres.sh             # Stop PostgreSQL
├── status_postgres.sh           # Check PostgreSQL status
├── run_vacuum_test.sh           # Run VACUUM test
├── test_vacuum_radix_tree.sql   # Main test SQL
├── test_vacuum_monitor.sql      # Monitoring SQL
├── VACUUM_TEST_README.md        # Detailed test documentation
├── SETUP_GUIDE.md              # This file
├── pg_install/                  # PostgreSQL installation
├── pg_data/                     # Database cluster
│   └── log/                     # PostgreSQL logs
└── src/                         # PostgreSQL source code
```

## 🔍 Understanding the Test Results

### What to Look For

After running `./run_vacuum_test.sh`, examine the VACUUM output:

**Key Metrics:**

```
automatic vacuum of table "postgres.public.test_vacuum_art":
  index scans: 1                    ← Number of index vacuum phases
  pages: 0 removed, 88496 remain, 88496 scanned (100.00% of total)
  tuples: 10000000 removed, 10000000 remain
  ...
  system usage: CPU: user: 7.82 s, system: 3.01 s, elapsed: 61.23 s
```

**In Postgres 17 specifically:**
- Look for `dead_tuple_bytes` instead of `num_dead_tuples`
- Fewer `index scans` compared to PG16
- Faster `elapsed` time

### Radix Tree Logging

With our added logging, you'll see entries like:

```
LOG:  Radix tree: inserted new key 12345
LOG:  Radix tree: updated existing key 67890
```

These show the TIDs (tuple identifiers) being stored in the adaptive radix tree.

## 📊 Scaling the Test

The default test uses 10M rows. To test with more data:

1. Edit `test_vacuum_radix_tree.sql`
2. Change this line:
   ```sql
   SELECT * FROM generate_series(1, 10000000) x(id);
   ```
   To:
   ```sql
   SELECT * FROM generate_series(1, 100000000) x(id);  -- 100M rows
   ```

**Expected results:**

| Rows | PG16 Index Scans | PG17 Index Scans | PG16 Time | PG17 Time |
|------|------------------|------------------|-----------|-----------|
| 10M  | 2-3              | 1                | ~80s      | ~60s      |
| 100M | 9                | 1                | ~770s     | ~620s     |

## 🛠️ Manual Build Steps (Alternative)

If you prefer to build manually:

```bash
# 1. Configure
./configure --prefix=/Users/cangokmen/Desktop/QuART-VACUUM/pg_install \
            --enable-debug --enable-cassert CFLAGS="-O0 -g3"

# 2. Compile
make -j$(sysctl -n hw.ncpu)

# 3. Install
make install

# 4. Initialize database
./pg_install/bin/initdb -D pg_data

# 5. Start PostgreSQL
./pg_install/bin/pg_ctl -D pg_data -l pg_data/logfile start

# 6. Run tests
./pg_install/bin/psql -d postgres -f test_vacuum_radix_tree.sql
```

## 🔧 Troubleshooting

### Build Issues

**Problem:** `configure: error: readline library not found`
```bash
# Install readline via Homebrew
brew install readline
./configure --with-includes=/opt/homebrew/opt/readline/include \
            --with-libraries=/opt/homebrew/opt/readline/lib \
            --prefix=/Users/cangokmen/Desktop/QuART-VACUUM/pg_install
```

**Problem:** `configure: error: ld: library not found for -lxml2`
```bash
# Install libxml2 via Homebrew
brew install libxml2
```

### Runtime Issues

**Problem:** PostgreSQL won't start
```bash
# Check logs
tail -f pg_data/log/postgresql-*.log

# Check if port 5432 is already in use
lsof -i :5432

# Try a different port
echo "port = 5433" >> pg_data/postgresql.conf
./start_postgres.sh
```

**Problem:** Out of memory during test
```bash
# Reduce test size in test_vacuum_radix_tree.sql
# Change to 1M or 5M rows instead of 10M
SELECT * FROM generate_series(1, 1000000) x(id);
```

**Problem:** Can't see VACUUM logs
```bash
# Check PostgreSQL configuration
grep log_autovacuum pg_data/postgresql.conf

# Verify logging is enabled
./pg_install/bin/psql -d postgres -c "SHOW log_autovacuum_min_duration;"

# Should show: 0
```

## 📖 Viewing Logs

PostgreSQL logs are in `pg_data/log/`:

```bash
# View latest log
ls -lt pg_data/log/ | head -1

# Tail logs in real-time
tail -f pg_data/log/postgresql-*.log

# Search for VACUUM logs
grep "automatic vacuum" pg_data/log/postgresql-*.log
```

## 🧪 Advanced Testing

### Test with Multiple Indexes

Add this to `test_vacuum_radix_tree.sql` after creating the first index:

```sql
CREATE INDEX test_vacuum_art_idx2 ON test_vacuum_art(id) WHERE id > 5000000;
CREATE INDEX test_vacuum_art_idx3 ON test_vacuum_art(id) WHERE id < 5000000;
```

With multiple indexes, the benefit of fewer index vacuum phases is even more pronounced!

### Test Different Memory Settings

Modify `maintenance_work_mem` in the test:

```sql
-- More memory = even fewer index vacuum phases
SET maintenance_work_mem = '128MB';  -- or '256MB', '512MB'
```

### Enable Detailed Timing

Add to `test_vacuum_radix_tree.sql`:

```sql
\timing on
```

This shows execution time for each SQL command.

## 📚 Additional Resources

- [VACUUM_TEST_README.md](VACUUM_TEST_README.md) - Detailed test documentation
- [PostgreSQL VACUUM Documentation](https://www.postgresql.org/docs/current/sql-vacuum.html)
- [pganalyze Blog Post](https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees)
- [Adaptive Radix Tree Paper](https://db.in.tum.de/~leis/papers/ART.pdf)

## 🎓 Understanding the Results

### Why This Matters

Before PostgreSQL 17, VACUUM was limited by how many dead tuple identifiers it could remember:
- Used a simple array
- ~11M tuples per 64MB of memory
- Had to do multiple "index vacuum passes" when it ran out of memory

With PostgreSQL 17's adaptive radix trees:
- More memory-efficient storage
- Can remember ~2x more dead tuples in the same memory
- Fewer index vacuum passes = significantly faster VACUUM
- Especially important for:
  - Tables with many dead tuples
  - Tables with multiple indexes
  - Systems with limited maintenance_work_mem

### Real-World Impact

For a production database with:
- Large tables (100M+ rows)
- Multiple indexes (5+ indexes)
- Regular updates causing dead tuples
- Limited maintenance window

PostgreSQL 17 can:
- Reduce VACUUM time by 20-30%
- Reduce I/O overhead from repeated index scans
- Allow autovacuum to keep up better with write-heavy workloads

## 🧹 Cleanup

To completely remove the test environment:

```bash
# Stop PostgreSQL
./stop_postgres.sh

# Remove data directory
rm -rf pg_data

# Remove installation
rm -rf pg_install

# Remove helper scripts
rm -f start_postgres.sh stop_postgres.sh status_postgres.sh

# Remove test table (if PostgreSQL is still running)
./pg_install/bin/psql -d postgres -c "DROP TABLE IF EXISTS test_vacuum_art CASCADE;"
```

## ✅ Verification Checklist

After completing the test, verify you saw:

- [ ] PostgreSQL compiled and started successfully
- [ ] Test table created with 10M rows
- [ ] Index created on the table
- [ ] All rows updated (creating dead tuples)
- [ ] VACUUM ran and completed
- [ ] VACUUM output shows index vacuum count
- [ ] Radix tree key insertion logs (if logging enabled)
- [ ] Test completed without errors

## 🤝 Contributing

Found an issue or improvement? This is a test environment for exploring PostgreSQL's adaptive radix tree implementation.

## 📝 Notes

- This builds PostgreSQL 17 from the current source
- The `pganalyze-example` branch may have additional modifications
- Logging has been added to track radix tree key insertions
- Build is configured with debugging enabled for development

Happy testing! 🚀
