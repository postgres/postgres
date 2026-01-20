#!/bin/bash
# Quick reference for PostgreSQL VACUUM testing commands

cat << 'EOF'
╔════════════════════════════════════════════════════════════════╗
║         PostgreSQL 17 VACUUM Test - Quick Reference           ║
╔════════════════════════════════════════════════════════════════╝

📦 INITIAL SETUP
─────────────────
  ./build_and_setup.sh          # Build & configure PostgreSQL (once)

🚀 START/STOP
─────────────
  ./start_postgres.sh           # Start PostgreSQL server
  ./stop_postgres.sh            # Stop PostgreSQL server
  ./status_postgres.sh          # Check if PostgreSQL is running

🧪 RUN TESTS
────────────
  ./run_vacuum_test.sh          # Run main VACUUM test (10M rows)

📊 MONITOR (run in separate terminal)
─────────────────────────────────────
  ./pg_install/bin/psql -d postgres -f test_vacuum_monitor.sql

🔍 MANUAL TESTING
─────────────────
  ./pg_install/bin/psql -d postgres              # Connect to database
  ./pg_install/bin/psql -d postgres -f FILE.sql  # Run SQL file

📝 VIEW LOGS
────────────
  tail -f pg_data/log/postgresql-*.log           # Follow PostgreSQL logs
  grep "automatic vacuum" pg_data/log/*.log      # Search for VACUUM logs
  ls -lt pg_data/log/ | head -5                  # List recent logs

🔧 USEFUL SQL COMMANDS (run inside psql)
────────────────────────────────────────
  # Check table size
  SELECT pg_size_pretty(pg_total_relation_size('test_vacuum_art'));

  # View dead tuples
  SELECT n_dead_tup FROM pg_stat_all_tables WHERE relname = 'test_vacuum_art';

  # Monitor vacuum progress
  SELECT * FROM pg_stat_progress_vacuum;

  # Check PostgreSQL version
  SHOW server_version;

  # View VACUUM settings
  SHOW maintenance_work_mem;
  SHOW autovacuum_work_mem;

  # Manual vacuum with verbose output
  VACUUM (VERBOSE, INDEX_CLEANUP ON) test_vacuum_art;

🧹 CLEANUP
──────────
  # Drop test table (in psql)
  DROP TABLE IF EXISTS test_vacuum_art CASCADE;

  # Remove all data and installation
  ./stop_postgres.sh
  rm -rf pg_data pg_install

📖 DOCUMENTATION
────────────────
  cat SETUP_GUIDE.md              # Full setup guide
  cat VACUUM_TEST_README.md       # Test documentation
  
  # Online resources
  open https://pganalyze.com/blog/5mins-postgres-17-faster-vacuum-adaptive-radix-trees

🎯 KEY METRICS TO WATCH
───────────────────────
  index_vacuum_count    # Lower is better (1 in PG17 vs 9 in PG16)
  elapsed time          # Faster in PG17 (~20% improvement)
  dead_tuple_bytes      # PG17: memory usage in bytes
  max_dead_tuples       # PG16: tuple count limit

💡 TIPS
───────
  1. Use multiple terminals: one for test, one for monitoring
  2. Check logs in real-time with tail -f
  3. Increase rows to 100M for full comparison (edit .sql file)
  4. Add multiple indexes to see greater benefits
  5. Try different maintenance_work_mem values (64MB, 128MB, 256MB)

EOF
