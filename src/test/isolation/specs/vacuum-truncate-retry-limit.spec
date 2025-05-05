# Test for VACUUM hitting the retry limit when truncation is repeatedly
# interrupted by conflicting locks during the backward scan phase.

setup
{
  CREATE TABLE vac_retry_tab (a int, b char(100));
  -- Need enough rows/pages to make truncation meaningful and the backward scan non-trivial
  INSERT INTO vac_retry_tab SELECT g, 'foo' FROM generate_series(1, 20000) g;
  ALTER TABLE vac_retry_tab SET (autovacuum_enabled = false);
  -- Delete most rows, leaving only the first few pages populated
  DELETE FROM vac_retry_tab WHERE a > 500;
  -- Initial vacuum to clean up dead rows, setting up for truncation test
  VACUUM vac_retry_tab;
}

teardown
{
  DROP TABLE vac_retry_tab;
}

session s1
step s1_vacuum { VACUUM (VERBOSE) vac_retry_tab; }
# This step implicitly waits for s1_vacuum to complete or error out
step s1_finish_check { SELECT 1; }

session s2
# Repeatedly take and release a lock that conflicts with AccessExclusiveLock
step s2_lock1 { BEGIN; LOCK vac_retry_tab IN ACCESS SHARE MODE; }
step s2_unlock1 { COMMIT; }
step s2_lock2 { BEGIN; LOCK vac_retry_tab IN ACCESS SHARE MODE; }
step s2_unlock2 { COMMIT; }
step s2_lock3 { BEGIN; LOCK vac_retry_tab IN ACCESS SHARE MODE; }
step s2_unlock3 { COMMIT; }
# This last lock might be held while VACUUM finally gives up or finishes
step s2_lock4 { BEGIN; LOCK vac_retry_tab IN ACCESS SHARE MODE; }
step s2_unlock4 { COMMIT; }

# The permutation aims to have s1 acquire the AccessExclusiveLock and start
# the count_nondeletable_pages scan, then s2 repeatedly interrupts it.
# s1_finish_check will wait until s1_vacuum finishes (either by completing
# truncation, hitting the retry limit, or erroring).
# We expect s1_vacuum to log the message about hitting the retry limit.
permutation s1_vacuum s2_lock1 s2_unlock1 s2_lock2 s2_unlock2 s2_lock3 s2_unlock3 s2_lock4 s1_finish_check s2_unlock4
