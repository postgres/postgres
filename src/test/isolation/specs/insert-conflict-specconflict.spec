# INSERT ... ON CONFLICT test verifying that speculative insertion
# failures are handled
#
# Does this by using advisory locks controlling progress of
# insertions. By waiting when building the index keys, it's possible
# to schedule concurrent INSERT ON CONFLICTs so that there will always
# be a speculative conflict.

setup
{
     CREATE OR REPLACE FUNCTION blurt_and_lock_123(text) RETURNS text IMMUTABLE LANGUAGE plpgsql AS $$
     BEGIN
        RAISE NOTICE 'blurt_and_lock_123() called for % in session %', $1, current_setting('spec.session')::int;

	-- depending on lock state, wait for lock 2 or 3
        IF pg_try_advisory_xact_lock(current_setting('spec.session')::int, 1) THEN
            RAISE NOTICE 'acquiring advisory lock on 2';
            PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 2);
        ELSE
            RAISE NOTICE 'acquiring advisory lock on 3';
            PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 3);
        END IF;
    RETURN $1;
    END;$$;

    CREATE OR REPLACE FUNCTION blurt_and_lock_4(text) RETURNS text IMMUTABLE LANGUAGE plpgsql AS $$
    BEGIN
        RAISE NOTICE 'blurt_and_lock_4() called for % in session %', $1, current_setting('spec.session')::int;
        RAISE NOTICE 'acquiring advisory lock on 4';
        PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 4);
    RETURN $1;
    END;$$;

    CREATE OR REPLACE FUNCTION ctoast_large_val() RETURNS text LANGUAGE SQL AS $$ select string_agg(encode(sha256(g::text::bytea),'hex'), '')::text from generate_series(1, 133) g $$;

    CREATE TABLE upserttest(key text, data text);

    CREATE UNIQUE INDEX upserttest_key_uniq_idx ON upserttest((blurt_and_lock_123(key)));
}

teardown
{
    DROP TABLE upserttest;
}

session controller
setup
{
    SET default_transaction_isolation = 'read committed';
}
step controller_locks {SELECT pg_advisory_lock(sess, lock), sess, lock FROM generate_series(1, 2) a(sess), generate_series(1,3) b(lock);}
step controller_unlock_1_1 { SELECT pg_advisory_unlock(1, 1); }
step controller_unlock_2_1 { SELECT pg_advisory_unlock(2, 1); }
step controller_unlock_1_2 { SELECT pg_advisory_unlock(1, 2); }
step controller_unlock_2_2 { SELECT pg_advisory_unlock(2, 2); }
step controller_unlock_1_3 { SELECT pg_advisory_unlock(1, 3); }
step controller_unlock_2_3 { SELECT pg_advisory_unlock(2, 3); }
step controller_lock_2_4 { SELECT pg_advisory_lock(2, 4); }
step controller_unlock_2_4 { SELECT pg_advisory_unlock(2, 4); }
step controller_show {SELECT * FROM upserttest; }
step controller_show_count {SELECT COUNT(*) FROM upserttest; }
step controller_print_speculative_locks {
    SELECT pa.application_name, locktype, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype IN ('spectoken', 'transactionid')
        AND pa.datname = current_database()
        AND pa.application_name LIKE 'isolation/insert-conflict-specconflict/s%'
    ORDER BY 1, 2, 3, 4;
}

session s1
setup
{
    SET default_transaction_isolation = 'read committed';
    SET spec.session = 1;
}
step s1_begin  { BEGIN; }
step s1_create_non_unique_index { CREATE INDEX upserttest_key_idx ON upserttest((blurt_and_lock_4(key))); }
step s1_confirm_index_order { SELECT 'upserttest_key_uniq_idx'::regclass::int8 < 'upserttest_key_idx'::regclass::int8; }
step s1_upsert { INSERT INTO upserttest(key, data) VALUES('k1', 'inserted s1') ON CONFLICT (blurt_and_lock_123(key)) DO UPDATE SET data = upserttest.data || ' with conflict update s1'; }
step s1_insert_toast { INSERT INTO upserttest VALUES('k2', ctoast_large_val()) ON CONFLICT DO NOTHING; }
step s1_commit  { COMMIT; }
step s1_noop    { }

session s2
setup
{
    SET default_transaction_isolation = 'read committed';
    SET spec.session = 2;
}
step s2_begin  { BEGIN; }
step s2_upsert { INSERT INTO upserttest(key, data) VALUES('k1', 'inserted s2') ON CONFLICT (blurt_and_lock_123(key)) DO UPDATE SET data = upserttest.data || ' with conflict update s2'; }
step s2_insert_toast { INSERT INTO upserttest VALUES('k2', ctoast_large_val()) ON CONFLICT DO NOTHING; }
step s2_commit  { COMMIT; }
step s2_noop    { }

# Test that speculative locks are correctly acquired and released, s2
# inserts, s1 updates.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock_123 function acquires advisory locks that allow us to
   # continue after a) the optimistic conflict probe b) after the
   # insertion of the speculative tuple.
   controller_locks
   controller_show
   s1_upsert s2_upsert
   controller_show
   # Switch both sessions to wait on the other lock next time (the speculative insertion)
   controller_unlock_1_1 controller_unlock_2_1
   # Allow both sessions to continue
   controller_unlock_1_3 controller_unlock_2_3
   controller_show
   # Allow the second session to finish insertion
   controller_unlock_2_2
   # This should now show a successful insertion
   controller_show
   # Allow the first session to finish insertion
   controller_unlock_1_2
   # This should now show a successful UPSERT
   controller_show

# Test that speculative locks are correctly acquired and released, s1
# inserts, s2 updates.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock_123 function acquires advisory locks that allow us to
   # continue after a) the optimistic conflict probe b) after the
   # insertion of the speculative tuple.
   controller_locks
   controller_show
   s1_upsert s2_upsert
   controller_show
   # Switch both sessions to wait on the other lock next time (the speculative insertion)
   controller_unlock_1_1 controller_unlock_2_1
   # Allow both sessions to continue
   controller_unlock_1_3 controller_unlock_2_3
   controller_show
   # Allow the first session to finish insertion
   controller_unlock_1_2
   # This should now show a successful insertion
   controller_show
   # Allow the second session to finish insertion
   controller_unlock_2_2
   # This should now show a successful UPSERT
   controller_show

# Test that speculatively inserted toast rows do not cause conflicts.
# s1 inserts successfully, s2 does not.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock_123 function acquires advisory locks that allow us to
   # continue after a) the optimistic conflict probe b) after the
   # insertion of the speculative tuple.
   controller_locks
   controller_show
   s1_insert_toast s2_insert_toast
   controller_show
   # Switch both sessions to wait on the other lock next time (the speculative insertion)
   controller_unlock_1_1 controller_unlock_2_1
   # Allow both sessions to continue
   controller_unlock_1_3 controller_unlock_2_3
   controller_show
   # Allow the first session to finish insertion
   controller_unlock_1_2
   # This should now show that 1 additional tuple was inserted successfully
   controller_show_count
   # Allow the second session to finish insertion and kill the speculatively inserted tuple
   controller_unlock_2_2
   # This should show the same number of tuples as before s2 inserted
   controller_show_count

# Test that speculative locks are correctly acquired and released, s2
# inserts, s1 updates.  With the added complication that transactions
# don't immediately commit.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock_123 function acquires advisory locks that allow us to
   # continue after a) the optimistic conflict probe b) after the
   # insertion of the speculative tuple.
   controller_locks
   controller_show
   s1_begin s2_begin
   s1_upsert s2_upsert
   controller_show
   # Switch both sessions to wait on the other lock next time (the speculative insertion)
   controller_unlock_1_1 controller_unlock_2_1
   # Allow both sessions to continue
   controller_unlock_1_3 controller_unlock_2_3
   controller_show
   # Allow the first session to finish insertion
   controller_unlock_1_2
   # But the change isn't visible yet, nor should the second session continue
   controller_show
   # Allow the second session to finish insertion, but it's blocked
   controller_unlock_2_2
   controller_show
   # But committing should unblock
   s1_commit
   controller_show
   s2_commit
   controller_show

# Test that speculative wait is performed if a session sees a speculatively
# inserted tuple. A speculatively inserted tuple is one which has been inserted
# both into the table and the unique index but has yet to *complete* the
# speculative insertion
permutation
   # acquire a number of advisory locks to control execution flow - the
   # blurt_and_lock_123 function acquires advisory locks that allow us to
   # continue after a) the optimistic conflict probe and b) after the
   # insertion of the speculative tuple.
   # blurt_and_lock_4 acquires an advisory lock which allows us to pause
   # execution c) before completing the speculative insertion

   # create the second index here to avoid affecting the other
   # permutations.
   s1_create_non_unique_index
   # confirm that the insertion into the unique index will happen first
   s1_confirm_index_order
   controller_locks
   controller_show
   s2_begin
   # Both sessions wait on advisory locks
   # (but don't show s2_upsert as complete till we've seen all of s1's notices)
   s1_upsert s2_upsert (s1_upsert notices 10)
   controller_show
   # Switch both sessions to wait on the other lock next time (the speculative insertion)
   controller_unlock_1_1 controller_unlock_2_1
   # Allow both sessions to do the optimistic conflict probe and do the
   # speculative insertion into the table
   # They will then be waiting on another advisory lock when they attempt to
   # update the index
   controller_unlock_1_3 controller_unlock_2_3
   controller_show
   # take lock to block second session after inserting in unique index but
   # before completing the speculative insert
   controller_lock_2_4
   # Allow the second session to move forward
   controller_unlock_2_2
   # This should still not show a successful insertion
   controller_show
   # Allow the first session to continue, it should perform speculative wait
   controller_unlock_1_2
   # Should report s1 is waiting on speculative lock
   controller_print_speculative_locks
   # Allow s2 to insert into the non-unique index and complete.  s1 will
   # no longer wait on speculative lock, but proceed to wait on the
   # transaction to finish.  The no-op step is needed to ensure that
   # we don't advance to the reporting step until s2_upsert has completed.
   controller_unlock_2_4 s2_noop
   # Should report that s1 is now waiting for s2 to commit
   controller_print_speculative_locks
   # Once s2 commits, s1 is finally free to continue to update
   s2_commit s1_noop
   # This should now show a successful UPSERT
   controller_show
   # Ensure no unexpected locks survive
   controller_print_speculative_locks
