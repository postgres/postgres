# Test for the lock statistics
#
# This test creates multiple locking situations when a session (s2) has to
# wait on a lock for longer than deadlock_timeout.  The first permutations
# test various lock tags.  The last permutation checks that log_lock_waits
# has no impact on the statistics counters.

setup
{
    CREATE TABLE test_stat_tab(key text not null, value int);
    INSERT INTO test_stat_tab(key, value) VALUES('k0', 1);
    SELECT pg_stat_force_next_flush();
}

teardown
{
    DROP TABLE IF EXISTS test_stat_tab;
}

session s1
setup { SET stats_fetch_consistency = 'none'; }
step s1_begin { BEGIN; }
step s1_commit { COMMIT; }
step s1_table_insert { INSERT INTO test_stat_tab(key, value) VALUES('k1', 1), ('k2', 1), ('k3', 1);}
step s1_table_update_k1 { UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k1';}
step s1_set_deadlock_timeout { SET deadlock_timeout = '10ms'; }
step s1_reset_stat_lock { SELECT pg_stat_reset_shared('lock'); }
step s1_sleep { SELECT pg_sleep(0.05); }
step s1_lock_relation { LOCK TABLE test_stat_tab; }
step s1_lock_advisory_lock { SELECT pg_advisory_lock(1); }
step s1_lock_advisory_unlock { SELECT pg_advisory_unlock(1); }

session s2
setup { SET stats_fetch_consistency = 'none'; }
step s2_begin { BEGIN; }
step s2_commit { COMMIT; }
step s2_ff { SELECT pg_stat_force_next_flush(); }
step s2_table_update_k1 { UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k1';}
step s2_set_deadlock_timeout { SET deadlock_timeout = '10ms'; }
step s2_set_log_lock_waits { SET log_lock_waits = on; }
step s2_unset_log_lock_waits { SET log_lock_waits = off; }
step s2_report_stat_lock_relation {
  SELECT waits > 0 AS has_waits, wait_time > 50 AS has_wait_time
    FROM pg_stat_lock WHERE locktype = 'relation';
}
step s2_report_stat_lock_transactionid {
  SELECT waits > 0 AS has_waits, wait_time > 50 AS has_wait_time
    FROM pg_stat_lock WHERE locktype = 'transactionid';
}
step s2_report_stat_lock_advisory {
  SELECT waits > 0 AS has_waits, wait_time > 50 AS has_wait_time
    FROM pg_stat_lock WHERE locktype = 'advisory';
}
step s2_lock_relation { LOCK TABLE test_stat_tab; }
step s2_lock_advisory_lock { SELECT pg_advisory_lock(1); }
step s2_lock_advisory_unlock { SELECT pg_advisory_unlock(1); }

######################
# Lock stats tests
######################

# relation lock

permutation
  s1_set_deadlock_timeout
  s1_reset_stat_lock
  s2_set_deadlock_timeout
  s1_begin
  s1_lock_relation
  s2_begin
  s2_ff
  s2_lock_relation
  s1_sleep
  s1_commit
  s2_commit
  s2_report_stat_lock_relation

# transaction lock

permutation
  s1_set_deadlock_timeout
  s1_reset_stat_lock
  s2_set_deadlock_timeout
  s2_set_log_lock_waits
  s1_table_insert
  s1_begin
  s1_table_update_k1
  s2_begin
  s2_ff
  s2_table_update_k1
  s1_sleep
  s1_commit
  s2_commit
  s2_report_stat_lock_transactionid

# advisory lock

permutation
  s1_set_deadlock_timeout
  s1_reset_stat_lock
  s2_set_deadlock_timeout
  s2_set_log_lock_waits
  s1_lock_advisory_lock
  s2_begin
  s2_ff
  s2_lock_advisory_lock
  s1_sleep
  s1_lock_advisory_unlock
  s2_lock_advisory_unlock
  s2_commit
  s2_report_stat_lock_advisory

# Ensure log_lock_waits has no impact

permutation
  s1_set_deadlock_timeout
  s1_reset_stat_lock
  s2_set_deadlock_timeout
  s2_unset_log_lock_waits
  s1_begin
  s1_lock_relation
  s2_begin
  s2_ff
  s2_lock_relation
  s1_sleep
  s1_commit
  s2_commit
  s2_report_stat_lock_relation
