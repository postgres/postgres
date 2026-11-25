# Test snapshot, cache and reset behavior for per-backend statistics.
#
# s2 generates WAL and flushes its statistics.  s1 reads s2's WAL stats with
# pg_stat_get_backend_wal().  Various patterns with transactions and
# values of stats_fetch_consistency are checked.

setup
{
  CREATE TABLE stats_per_backend_data(id int);
  CREATE TABLE stats_per_backend_saved(wal_records bigint);
}

teardown
{
  DROP TABLE stats_per_backend_data;
  DROP TABLE stats_per_backend_saved;
}

session s1
setup { SET stats_fetch_consistency = 'none'; }

step s1_fetch_consistency_none { SET stats_fetch_consistency = 'none'; }
step s1_fetch_consistency_cache { SET stats_fetch_consistency = 'cache'; }
step s1_fetch_consistency_snapshot { SET stats_fetch_consistency = 'snapshot'; }
step s1_begin { BEGIN; }
step s1_commit { COMMIT; }

# Under snapshot mode, the first fetch of any kind builds the snapshot
# of all entries, including per-backend ones.
step s1_build_snapshot {
  SELECT wal_records >= 0 AS snapshot_created FROM pg_stat_wal;
}

# Save s2's WAL stats, to be compared later with s1_check_backend.
step s1_save_backend_stats {
  INSERT INTO stats_per_backend_saved
  SELECT wal_records
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-backend/s2'));
}

step s1_check_backend {
  SELECT current_setting('stats_fetch_consistency') AS fetch_consistency,
         current.wal_records = saved.wal_records AS unchanged,
         current.wal_records > saved.wal_records AS advanced
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-backend/s2')) AS current
  CROSS JOIN stats_per_backend_saved AS saved;
}

# Reset s2's WAL stats.
step s1_reset_backend {
  SELECT pg_stat_reset_backend_stats(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-backend/s2'));
}
step s1_check_reset {
  SELECT stats_reset IS NOT NULL AS reset_timestamp_set
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-backend/s2'));
}

session s2
step s2_generate {
  INSERT INTO stats_per_backend_data VALUES (1);
  SELECT pg_stat_force_next_flush();
}

# With stats_fetch_consistency=none, s1 sees each flush from s2, even within
# a transaction.
permutation
  s1_fetch_consistency_none
  s2_generate
  s1_begin
  s1_save_backend_stats
  s2_generate
  s1_check_backend
  s1_commit

# With stats_fetch_consistency=snapshot, s1 does not see any flushed changes
# from s2 after building its statistics snapshot; s1 should see the flushes
# of s2 after commit.
permutation
  s1_fetch_consistency_snapshot
  s1_begin
  s1_build_snapshot
  s2_generate
  s1_save_backend_stats
  s1_commit
  s1_check_backend

# With stats_fetch_consistency=cache, s1 keeps s2's data as of what has
# been retrieved during first access, until commit.
permutation
  s1_fetch_consistency_cache
  s2_generate
  s1_begin
  s1_save_backend_stats
  s2_generate
  s1_check_backend
  s1_commit
  s1_check_backend

# Switching from "cache" to "snapshot" within a transaction clears the
# cached entry.  The new snapshot sees s2's latest flush.
permutation
  s1_fetch_consistency_cache
  s2_generate
  s1_begin
  s1_save_backend_stats
  s2_generate
  s1_fetch_consistency_snapshot
  s1_check_backend
  s1_commit

# Resetting s2's stats resets its timestamp.
permutation s1_check_reset s1_reset_backend s1_check_reset
