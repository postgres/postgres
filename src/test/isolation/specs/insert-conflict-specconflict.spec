# INSERT ... ON CONFLICT test verifying that speculative insertion
# failures are handled
#
# Does this by using advisory locks controlling progress of
# insertions. By waiting when building the index keys, it's possible
# to schedule concurrent INSERT ON CONFLICTs so that there will always
# be a speculative conflict.

setup
{
     CREATE OR REPLACE FUNCTION blurt_and_lock(text) RETURNS text IMMUTABLE LANGUAGE plpgsql AS $$
     BEGIN
        RAISE NOTICE 'called for %', $1;

	-- depending on lock state, wait for lock 2 or 3
        IF pg_try_advisory_xact_lock(current_setting('spec.session')::int, 1) THEN
            RAISE NOTICE 'blocking 2';
            PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 2);
        ELSE
            RAISE NOTICE 'blocking 3';
            PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 3);
        END IF;
    RETURN $1;
    END;$$;

    CREATE TABLE upserttest(key text, data text);

    CREATE UNIQUE INDEX ON upserttest((blurt_and_lock(key)));
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
step controller_show {SELECT * FROM upserttest; }

session s1
setup
{
  SET default_transaction_isolation = 'read committed';
  SET spec.session = 1;
}
step s1_begin  { BEGIN; }
step s1_upsert { INSERT INTO upserttest(key, data) VALUES('k1', 'inserted s1') ON CONFLICT (blurt_and_lock(key)) DO UPDATE SET data = upserttest.data || ' with conflict update s1'; }
step s1_commit  { COMMIT; }

session s2
setup
{
  SET default_transaction_isolation = 'read committed';
  SET spec.session = 2;
}
step s2_begin  { BEGIN; }
step s2_upsert { INSERT INTO upserttest(key, data) VALUES('k1', 'inserted s2') ON CONFLICT (blurt_and_lock(key)) DO UPDATE SET data = upserttest.data || ' with conflict update s2'; }
step s2_commit  { COMMIT; }

# Test that speculative locks are correctly acquired and released, s2
# inserts, s1 updates.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock function acquires advisory locks that allow us to
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

# Test that speculative locks are correctly acquired and released, s2
# inserts, s1 updates.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock function acquires advisory locks that allow us to
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

# Test that speculative locks are correctly acquired and released, s2
# inserts, s1 updates.  With the added complication that transactions
# don't immediately commit.
permutation
   # acquire a number of locks, to control execution flow - the
   # blurt_and_lock function acquires advisory locks that allow us to
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
