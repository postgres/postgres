setup
{
    CREATE TABLE test_stat_oid(name text NOT NULL, oid oid);

    CREATE TABLE test_stat_tab(key text not null, value int);
    INSERT INTO test_stat_tab(key, value) VALUES('k0', 1);
    INSERT INTO test_stat_oid(name, oid) VALUES('test_stat_tab', 'test_stat_tab'::regclass);

    CREATE FUNCTION test_stat_func() RETURNS VOID LANGUAGE plpgsql AS $$BEGIN END;$$;
    INSERT INTO test_stat_oid(name, oid) VALUES('test_stat_func', 'test_stat_func'::regproc);

    CREATE FUNCTION test_stat_func2() RETURNS VOID LANGUAGE plpgsql AS $$BEGIN END;$$;
    INSERT INTO test_stat_oid(name, oid) VALUES('test_stat_func2', 'test_stat_func2'::regproc);

    CREATE TABLE test_slru_stats(slru TEXT, stat TEXT, value INT);

    -- calls test_stat_func, but hides error if it doesn't exist
    CREATE FUNCTION test_stat_func_ifexists() RETURNS VOID LANGUAGE plpgsql AS $$
    BEGIN
        PERFORM test_stat_func();
    EXCEPTION WHEN undefined_function THEN
    END;$$;

    SELECT pg_stat_force_next_flush();
}

teardown
{
    DROP TABLE test_stat_oid;
    DROP TABLE test_slru_stats;

    DROP TABLE IF EXISTS test_stat_tab;
    DROP FUNCTION IF EXISTS test_stat_func();
    DROP FUNCTION IF EXISTS test_stat_func2();
    DROP FUNCTION test_stat_func_ifexists();
}

session s1
setup { SET stats_fetch_consistency = 'none'; }
step s1_fetch_consistency_none { SET stats_fetch_consistency = 'none'; }
step s1_fetch_consistency_cache { SET stats_fetch_consistency = 'cache'; }
step s1_fetch_consistency_snapshot { SET stats_fetch_consistency = 'snapshot'; }
step s1_clear_snapshot { SELECT pg_stat_clear_snapshot(); }
step s1_begin { BEGIN; }
step s1_commit { COMMIT; }
step s1_rollback { ROLLBACK; }
step s1_prepare_a { PREPARE TRANSACTION 'a'; }
step s1_commit_prepared_a { COMMIT PREPARED 'a'; }
step s1_rollback_prepared_a { ROLLBACK PREPARED 'a'; }

# Function stats steps
step s1_ff { SELECT pg_stat_force_next_flush(); }
step s1_track_funcs_all { SET track_functions = 'all'; }
step s1_track_funcs_none { SET track_functions = 'none'; }
step s1_func_call { SELECT test_stat_func(); }
step s1_func_drop { DROP FUNCTION test_stat_func(); }
step s1_func_stats_reset { SELECT pg_stat_reset_single_function_counters('test_stat_func'::regproc); }
step s1_func_stats_reset_nonexistent { SELECT pg_stat_reset_single_function_counters(12000); }
step s1_reset { SELECT pg_stat_reset(); }
step s1_func_stats {
    SELECT
        tso.name,
        pg_stat_get_function_calls(tso.oid),
        pg_stat_get_function_total_time(tso.oid) > 0 total_above_zero,
        pg_stat_get_function_self_time(tso.oid) > 0 self_above_zero
    FROM test_stat_oid AS tso
    WHERE tso.name = 'test_stat_func'
}
step s1_func_stats2 {
    SELECT
        tso.name,
        pg_stat_get_function_calls(tso.oid),
        pg_stat_get_function_total_time(tso.oid) > 0 total_above_zero,
        pg_stat_get_function_self_time(tso.oid) > 0 self_above_zero
    FROM test_stat_oid AS tso
    WHERE tso.name = 'test_stat_func2'
}
step s1_func_stats_nonexistent {
    SELECT pg_stat_get_function_calls(12000);
}

# Relation stats steps
step s1_track_counts_on { SET track_counts = on; }
step s1_track_counts_off { SET track_counts = off; }
step s1_table_select { SELECT * FROM test_stat_tab ORDER BY key, value; }
step s1_table_insert { INSERT INTO test_stat_tab(key, value) VALUES('k1', 1), ('k2', 1), ('k3', 1);}
step s1_table_insert_k1 { INSERT INTO test_stat_tab(key, value) VALUES('k1', 1);}
step s1_table_update_k1 { UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k1';}
step s1_table_update_k2 { UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k2';}
step s1_table_delete_k1 { DELETE FROM test_stat_tab WHERE key = 'k1';}
step s1_table_truncate { TRUNCATE test_stat_tab; }
step s1_table_drop { DROP TABLE test_stat_tab; }

step s1_table_stats {
    SELECT
        pg_stat_get_numscans(tso.oid) AS seq_scan,
        pg_stat_get_tuples_returned(tso.oid) AS seq_tup_read,
        pg_stat_get_tuples_inserted(tso.oid) AS n_tup_ins,
        pg_stat_get_tuples_updated(tso.oid) AS n_tup_upd,
        pg_stat_get_tuples_deleted(tso.oid) AS n_tup_del,
        pg_stat_get_live_tuples(tso.oid) AS n_live_tup,
        pg_stat_get_dead_tuples(tso.oid) AS n_dead_tup,
        pg_stat_get_vacuum_count(tso.oid) AS vacuum_count
    FROM test_stat_oid AS tso
    WHERE tso.name = 'test_stat_tab'
}

# SLRU stats steps
step s1_slru_save_stats {
	INSERT INTO test_slru_stats VALUES('Notify', 'blks_zeroed',
    (SELECT blks_zeroed FROM pg_stat_slru WHERE name = 'Notify'));
}
step s1_listen { LISTEN stats_test_nothing; }
step s1_big_notify { SELECT pg_notify('stats_test_use',
                repeat(i::text, current_setting('block_size')::int / 2)) FROM generate_series(1, 3) g(i);
                }

step s1_slru_check_stats {
	SELECT current.blks_zeroed > before.value
  FROM test_slru_stats before
  INNER JOIN pg_stat_slru current
  ON before.slru = current.name
  WHERE before.stat = 'blks_zeroed';
	}


session s2
setup { SET stats_fetch_consistency = 'none'; }
step s2_begin { BEGIN; }
step s2_commit { COMMIT; }
step s2_commit_prepared_a { COMMIT PREPARED 'a'; }
step s2_rollback_prepared_a { ROLLBACK PREPARED 'a'; }
step s2_ff { SELECT pg_stat_force_next_flush(); }

# Function stats steps
step s2_track_funcs_all { SET track_functions = 'all'; }
step s2_track_funcs_none { SET track_functions = 'none'; }
step s2_func_call { SELECT test_stat_func() }
step s2_func_call_ifexists { SELECT test_stat_func_ifexists(); }
step s2_func_call2 { SELECT test_stat_func2() }
step s2_func_stats {
    SELECT
        tso.name,
        pg_stat_get_function_calls(tso.oid),
        pg_stat_get_function_total_time(tso.oid) > 0 total_above_zero,
        pg_stat_get_function_self_time(tso.oid) > 0 self_above_zero
    FROM test_stat_oid AS tso
    WHERE tso.name = 'test_stat_func'
}

# Relation stats steps
step s2_table_select { SELECT * FROM test_stat_tab ORDER BY key, value; }
step s2_table_update_k1 { UPDATE test_stat_tab SET value = value + 1 WHERE key = 'k1';}

# SLRU stats steps
step s2_big_notify { SELECT pg_notify('stats_test_use',
                repeat(i::text, current_setting('block_size')::int / 2)) FROM generate_series(1, 3) g(i);
                }


######################
# Function stats tests
######################

# check that stats are collected iff enabled
permutation
  s1_track_funcs_none s1_func_stats s1_func_call s1_func_call s1_ff s1_func_stats
permutation
  s1_track_funcs_all s1_func_stats s1_func_call s1_func_call s1_ff s1_func_stats

# multiple function calls are accurately reported, across separate connections
permutation
  s1_track_funcs_all s2_track_funcs_all s1_func_stats s2_func_stats
  s1_func_call s2_func_call s1_func_call s2_func_call s2_func_call s1_ff s2_ff s1_func_stats s2_func_stats
permutation
  s1_track_funcs_all s2_track_funcs_all s1_func_stats s2_func_stats
  s1_func_call s1_ff s2_func_call s2_func_call s2_ff s1_func_stats s2_func_stats
permutation
  s1_track_funcs_all s2_track_funcs_all s1_func_stats s2_func_stats
  s1_begin s1_func_call s1_func_call s1_commit s1_ff s1_func_stats s2_func_stats


### Check interaction between dropping and stats reporting

# Some of these tests try to test behavior in cases where no invalidation
# processing is triggered. To prevent output changes when
# debug_discard_caches, CATCACHE_FORCE_RELEASE or RELCACHE_FORCE_RELEASE are
# used (which trigger invalidation processing in paths that normally don't),
# test_stat_func_ifexists() can be used, which tries to call test_stat_func(),
# but doesn't raise an error if the function doesn't exist.

# dropping a table remove stats iff committed
permutation
  s1_track_funcs_all s2_track_funcs_all s1_func_stats s2_func_stats
  s1_begin s1_func_call s2_func_call s1_func_drop s2_func_call s2_ff s2_func_stats s1_commit s1_ff s1_func_stats s2_func_stats
permutation
  s1_track_funcs_all s2_track_funcs_all s1_func_stats s2_func_stats
  s1_begin s1_func_call s2_func_call s1_func_drop s2_func_call s2_ff s2_func_stats s1_rollback s1_ff s1_func_stats s2_func_stats

# Verify that pending stats from before a drop do not lead to
# reviving stats for a dropped object
permutation
  s1_track_funcs_all s2_track_funcs_all
  s2_func_call s2_ff # this access increments refcount, preventing the shared entry from being dropped
  s2_begin s2_func_call_ifexists s1_func_drop s1_func_stats s2_commit s2_ff s1_func_stats s2_func_stats
permutation
  s1_track_funcs_all s2_track_funcs_all
  s2_begin s2_func_call_ifexists s1_func_drop s1_func_stats s2_commit s2_ff s1_func_stats s2_func_stats
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_func_call s2_begin s2_func_call_ifexists s1_func_drop s2_func_call_ifexists s2_commit s2_ff s1_func_stats s2_func_stats

# Function calls don't necessarily trigger cache invalidation processing. The
# default handling of dropped stats could therefore end up with stats getting
# revived by a function call done after stats processing - but
# pgstat_init_function_usage() protects against that if track_functions is
# on. Verify that the stats are indeed dropped, and document the behavioral
# difference between track_functions settings.
permutation
  s1_track_funcs_all s2_track_funcs_none
  s1_func_call s2_begin s2_func_call_ifexists s1_ff s1_func_stats s1_func_drop s2_track_funcs_none s1_func_stats s2_func_call_ifexists s2_commit s2_ff s1_func_stats s2_func_stats
permutation
  s1_track_funcs_all s2_track_funcs_none
  s1_func_call s2_begin s2_func_call_ifexists s1_ff s1_func_stats s1_func_drop s2_track_funcs_all s1_func_stats s2_func_call_ifexists s2_commit s2_ff s1_func_stats s2_func_stats

# test pg_stat_reset_single_function_counters
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_func_call
  s2_func_call
  s2_func_call2
  s1_ff s2_ff
  s1_func_stats
  s2_func_call s2_func_call2 s2_ff
  s1_func_stats s1_func_stats2 s1_func_stats
  s1_func_stats_reset
  s1_func_stats s1_func_stats2 s1_func_stats

# test pg_stat_reset_single_function_counters of non-existing function
permutation
  s1_func_stats_nonexistent
  s1_func_stats_reset_nonexistent
  s1_func_stats_nonexistent

# test pg_stat_reset
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_func_call
  s2_func_call
  s2_func_call2
  s1_ff s2_ff
  s1_func_stats s1_func_stats2 s1_func_stats
  s1_reset
  s1_func_stats s1_func_stats2 s1_func_stats


### Check the different snapshot consistency models

# First just some dead-trivial test verifying each model doesn't crash
permutation
  s1_track_funcs_all s1_fetch_consistency_none s1_func_call s1_ff s1_func_stats
permutation
  s1_track_funcs_all s1_fetch_consistency_cache s1_func_call s1_ff s1_func_stats
permutation
  s1_track_funcs_all s1_fetch_consistency_snapshot s1_func_call s1_ff s1_func_stats

# with stats_fetch_consistency=none s1 should see flushed changes in s2, despite being in a transaction
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_fetch_consistency_none
  s2_func_call s2_ff
  s1_begin
  s1_func_stats
  s2_func_call s2_ff
  s1_func_stats
  s1_commit

# with stats_fetch_consistency=cache s1 should not see concurrent
# changes to the same object after the first access, but a separate
# object should show changes
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_fetch_consistency_cache
  s2_func_call s2_func_call2 s2_ff
  s1_begin
  s1_func_stats
  s2_func_call s2_func_call2 s2_ff
  s1_func_stats s1_func_stats2
  s1_commit

# with stats_fetch_consistency=snapshot s1 should not see any
# concurrent changes after the first access
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_fetch_consistency_snapshot
  s2_func_call s2_func_call2 s2_ff
  s1_begin
  s1_func_stats
  s2_func_call s2_func_call2 s2_ff
  s1_func_stats s1_func_stats2
  s1_commit

# Check access to non-existing stats works correctly and repeatedly
permutation
  s1_fetch_consistency_none
  s1_begin
  s1_func_stats_nonexistent
  s1_func_stats_nonexistent
  s1_commit
permutation
  s1_fetch_consistency_cache
  s1_begin
  s1_func_stats_nonexistent
  s1_func_stats_nonexistent
  s1_commit
permutation
  s1_fetch_consistency_snapshot
  s1_begin
  s1_func_stats_nonexistent
  s1_func_stats_nonexistent
  s1_commit


### Check 2PC handling of stat drops

# S1 prepared, S1 commits prepared
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_begin
  s1_func_call
  s2_func_call
  s1_func_drop
  s2_func_call
  s2_ff
  s1_prepare_a
  s2_func_call
  s2_ff
  s1_func_call
  s1_ff
  s1_func_stats
  s1_commit_prepared_a
  s1_func_stats

# S1 prepared, S1 aborts prepared
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_begin
  s1_func_call
  s2_func_call
  s1_func_drop
  s2_func_call
  s2_ff
  s1_prepare_a
  s2_func_call
  s2_ff
  s1_func_call
  s1_ff
  s1_func_stats
  s1_rollback_prepared_a
  s1_func_stats

# S1 prepares, S2 commits prepared
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_begin
  s1_func_call
  s2_func_call
  s1_func_drop
  s2_func_call
  s2_ff
  s1_prepare_a
  s2_func_call
  s2_ff
  s1_func_call
  s1_ff
  s1_func_stats
  s2_commit_prepared_a
  s1_func_stats

# S1 prepared, S2 aborts prepared
permutation
  s1_track_funcs_all s2_track_funcs_all
  s1_begin
  s1_func_call
  s2_func_call
  s1_func_drop
  s2_func_call
  s2_ff
  s1_prepare_a
  s2_func_call
  s2_ff
  s1_func_call
  s1_ff
  s1_func_stats
  s2_rollback_prepared_a
  s1_func_stats


######################
# Table stats tests
######################

# Most of the stats handling mechanism has already been tested in the function
# stats tests above - that's cheaper than testing with relations. But
# particularly for 2PC there are special cases


### Verify that pending stats from before a drop do not lead to reviving
### of stats for a dropped object

permutation
  s1_table_select
  s1_table_insert
  s2_table_select
  s2_table_update_k1
  s1_ff
  s2_table_update_k1
  s1_table_drop
  s2_ff
  s1_table_stats

permutation
  s1_table_select
  s1_table_insert
  s2_table_select
  s2_table_update_k1
  s2_table_update_k1
  s1_table_drop
  s1_table_stats


### Check that we don't count changes with track counts off, but allow access
### to prior stats

# simple read access with stats off
permutation
  s1_track_counts_off
  s1_table_stats
  s1_track_counts_on

# simple read access with stats off, previously accessed
permutation
  s1_table_select
  s1_track_counts_off
  s1_ff
  s1_table_stats
  s1_track_counts_on
permutation
  s1_table_select
  s1_ff
  s1_track_counts_off
  s1_table_stats
  s1_track_counts_on

# ensure we don't count anything with stats off
permutation
  s1_track_counts_off
  s1_table_select
  s1_table_insert_k1
  s1_table_update_k1
  s2_table_select
  s1_track_counts_on
  s1_ff s2_ff
  s1_table_stats
  # but can count again after
  s1_table_select
  s1_table_update_k1
  s1_ff
  s1_table_stats
permutation
  s1_table_select
  s1_table_insert_k1
  s1_table_delete_k1
  s1_track_counts_off
  s1_table_select
  s1_table_insert_k1
  s1_table_update_k1
  s2_table_select
  s1_track_counts_on
  s1_ff s2_ff
  s1_table_stats
  s1_table_select
  s1_table_update_k1
  s1_ff
  s1_table_stats


### 2PC: transactional and non-transactional counters work correctly

# S1 prepares, S2 commits prepared
permutation
  s1_begin
  s1_table_insert s1_table_update_k1 s1_table_update_k1 s1_table_update_k2 s1_table_update_k2 s1_table_update_k2 s1_table_delete_k1
  s1_table_select
  s1_prepare_a
  s1_table_select
  s1_commit_prepared_a
  s1_table_select
  s1_ff
  s1_table_stats

# S1 prepares, S2 commits prepared
permutation
  s1_begin
  s1_table_insert s1_table_update_k1 s1_table_update_k1 s1_table_update_k2 s1_table_update_k2 s1_table_update_k2 s1_table_delete_k1
  s1_table_select
  s1_prepare_a
  s1_table_select
  s2_commit_prepared_a
  s1_table_select
  s1_ff s2_ff
  s1_table_stats

# S1 prepares, S2 commits prepared
permutation
  s1_begin
  s1_table_insert s1_table_update_k1 s1_table_update_k1 s1_table_update_k2 s1_table_update_k2 s1_table_update_k2 s1_table_delete_k1
  s1_table_select
  s1_prepare_a
  s1_table_select
  s1_rollback_prepared_a
  s1_table_select
  s1_ff
  s1_table_stats

# S1 prepares, S1 aborts prepared
permutation
  s1_begin
  s1_table_insert s1_table_update_k1 s1_table_update_k1 s1_table_update_k2 s1_table_update_k2 s1_table_update_k2 s1_table_delete_k1
  s1_table_select
  s1_prepare_a
  s1_table_select
  s2_rollback_prepared_a
  s1_table_select
  s1_ff s2_ff
  s1_table_stats


### 2PC: truncate handling

# S1 prepares, S1 commits prepared
permutation
  s1_table_insert
  s1_begin
  s1_table_update_k1 # should *not* be counted, different rel
  s1_table_update_k1 # dito
  s1_table_truncate
  s1_table_insert_k1 # should be counted
  s1_table_update_k1 # dito
  s1_prepare_a
  s1_commit_prepared_a
  s1_ff
  s1_table_stats

# S1 prepares, S2 commits prepared
permutation
  s1_table_insert
  s1_begin
  s1_table_update_k1 # should *not* be counted, different rel
  s1_table_update_k1 # dito
  s1_table_truncate
  s1_table_insert_k1 # should be counted
  s1_table_update_k1 # dito
  s1_prepare_a
  s1_ff # flush out non-transactional stats, might happen anyway
  s2_commit_prepared_a
  s2_ff
  s1_table_stats

# S1 prepares, S1 aborts prepared
permutation
  s1_table_insert
  s1_begin
  s1_table_update_k1 # should be counted
  s1_table_update_k1 # dito
  s1_table_truncate
  s1_table_insert_k1 # should *not* be counted, different rel
  s1_table_update_k1 # dito
  s1_prepare_a
  s1_rollback_prepared_a
  s1_ff
  s1_table_stats

# S1 prepares, S2 aborts prepared
permutation
  s1_table_insert
  s1_begin
  s1_table_update_k1 # should be counted
  s1_table_update_k1 # dito
  s1_table_truncate
  s1_table_insert_k1 # should *not* be counted, different rel
  s1_table_update_k1 # dito
  s1_prepare_a
  s2_rollback_prepared_a
  s1_ff s2_ff
  s1_table_stats


### 2PC: rolled back drop maintains live / dead counters

# S1 prepares, S1 aborts prepared
permutation
  s1_table_insert
  s1_table_update_k1
  s1_begin
  # should all be counted
  s1_table_delete_k1
  s1_table_insert_k1
  s1_table_update_k1
  s1_table_update_k1
  s1_table_drop
  s1_prepare_a
  s1_rollback_prepared_a
  s1_ff
  s1_table_stats

# S1 prepares, S1 aborts prepared
permutation
  s1_table_insert
  s1_table_update_k1
  s1_begin
  # should all be counted
  s1_table_delete_k1
  s1_table_insert_k1
  s1_table_update_k1
  s1_table_update_k1
  s1_table_drop
  s1_prepare_a
  s2_rollback_prepared_a
  s1_ff s2_ff
  s1_table_stats


######################
# SLRU stats tests
######################

# Verify SLRU stats generated in own transaction
permutation
  s1_slru_save_stats
  s1_listen
  s1_begin
  s1_big_notify
  s1_ff
  s1_slru_check_stats
  s1_commit
  s1_slru_check_stats

# Verify SLRU stats generated in separate transaction
permutation
  s1_slru_save_stats
  s1_listen
  s2_big_notify
  s2_ff
  s1_slru_check_stats

# shouldn't see stats yet, not committed
permutation
  s1_slru_save_stats
  s1_listen
  s2_begin
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s2_commit


### Check the different snapshot consistency models for fixed-amount statistics

permutation
  s1_fetch_consistency_none
  s1_slru_save_stats s1_listen
  s1_begin
  s1_slru_check_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_commit
  s1_slru_check_stats
permutation
  s1_fetch_consistency_cache
  s1_slru_save_stats s1_listen
  s1_begin
  s1_slru_check_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_commit
  s1_slru_check_stats
permutation
  s1_fetch_consistency_snapshot
  s1_slru_save_stats s1_listen
  s1_begin
  s1_slru_check_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_commit
  s1_slru_check_stats

# check that pg_stat_clear_snapshot(), well ...
permutation
  s1_fetch_consistency_none
  s1_slru_save_stats s1_listen
  s1_begin
  s1_slru_check_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_clear_snapshot
  s1_slru_check_stats
  s1_commit
permutation
  s1_fetch_consistency_cache
  s1_slru_save_stats s1_listen
  s1_begin
  s1_slru_check_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_clear_snapshot
  s1_slru_check_stats
  s1_commit
permutation
  s1_fetch_consistency_snapshot
  s1_slru_save_stats s1_listen
  s1_begin
  s1_slru_check_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_clear_snapshot
  s1_slru_check_stats
  s1_commit

# check that a variable-amount stats access caches fixed-amount stat too
permutation
  s1_fetch_consistency_snapshot
  s1_slru_save_stats s1_listen
  s1_begin
  s1_func_stats
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s1_commit

# and the other way round
permutation
  s1_fetch_consistency_snapshot
  s1_slru_save_stats s1_listen
  s1_begin
  s2_big_notify
  s2_ff
  s1_slru_check_stats
  s2_func_call
  s2_ff
  s1_func_stats
  s1_clear_snapshot
  s1_func_stats
  s1_commit
