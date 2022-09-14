# Subtransaction overflow
#
# This test is designed to cover some code paths which only occur when
# one transaction has overflowed the subtransaction cache.

setup
{
DROP TABLE IF EXISTS subxids;
CREATE TABLE subxids (subx integer, val integer);

CREATE OR REPLACE FUNCTION gen_subxids (n integer)
 RETURNS VOID
 LANGUAGE plpgsql
AS $$
BEGIN
  IF n <= 0 THEN
	UPDATE subxids SET val = 1 WHERE subx = 0;
    RETURN;
  ELSE
    PERFORM gen_subxids(n - 1);
    RETURN;
  END IF;
EXCEPTION /* generates a subxid */
  WHEN raise_exception THEN NULL;
END;
$$;
}

teardown
{
 DROP TABLE subxids;
 DROP FUNCTION gen_subxids(integer);
}

session s1
# setup step for each test
step ins	{ TRUNCATE subxids; INSERT INTO subxids VALUES (0, 0); }
# long running transaction with overflowed subxids
step subxov	{ BEGIN; SELECT gen_subxids(100); }
# commit should always come last to make this long running
step s1c	{ COMMIT; }

session s2
# move xmax forwards
step xmax	{ BEGIN; INSERT INTO subxids VALUES (99, 0); COMMIT;}

# step for test1
step s2sel	{ SELECT val FROM subxids WHERE subx = 0; }

# steps for test2
step s2brr { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2brc { BEGIN ISOLATION LEVEL READ COMMITTED; }
# look for data written by sub3
step s2s3	{ SELECT val FROM subxids WHERE subx = 1; }
step s2c	{ COMMIT; }

# step for test3
step s2upd	{ UPDATE subxids SET val = 1 WHERE subx = 0; }

session s3
# transaction with subxids that can commit before s1c
step sub3	{ BEGIN; SAVEPOINT s; INSERT INTO subxids VALUES (1, 0); }
step s3c	{ COMMIT; }

# test1
# s2sel will see subxid as still running
# designed to test XidInMVCCSnapshot() when overflows, xid is found
permutation ins subxov xmax s2sel s1c

# test2
# designed to test XidInMVCCSnapshot() when overflows, xid is not found
# both SELECTs invisible
permutation ins subxov sub3 xmax s2brr s2s3 s3c s2s3 s2c s1c
# 2nd SELECT visible after commit
permutation ins subxov sub3 xmax s2brc s2s3 s3c s2s3 s2c s1c

# test3
# designed to test XactLockTableWait() for overflows
permutation ins subxov xmax s2upd s1c
