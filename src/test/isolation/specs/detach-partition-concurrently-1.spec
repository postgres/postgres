# Test that detach partition concurrently makes the partition invisible at the
# correct time.

setup
{
  DROP TABLE IF EXISTS d_listp, d_listp1, d_listp2;
  CREATE TABLE d_listp (a int) PARTITION BY LIST(a);
  CREATE TABLE d_listp1 PARTITION OF d_listp FOR VALUES IN (1);
  CREATE TABLE d_listp2 PARTITION OF d_listp FOR VALUES IN (2);
  INSERT INTO d_listp VALUES (1),(2);
}

teardown {
    DROP TABLE IF EXISTS d_listp, d_listp2, d_listp_foobar;
}

session s1
step s1b		{ BEGIN; }
step s1brr		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1s		{ SELECT * FROM d_listp; }
step s1ins		{ INSERT INTO d_listp VALUES (1); }
step s1ins2		{ INSERT INTO d_listp VALUES (2); }
step s1prep		{ PREPARE f(int) AS INSERT INTO d_listp VALUES ($1); }
step s1prep1	{ PREPARE f(int) AS INSERT INTO d_listp VALUES (1); }
step s1prep2	{ PREPARE f(int) AS INSERT INTO d_listp VALUES (2); }
step s1exec1	{ EXECUTE f(1); }
step s1exec2	{ EXECUTE f(2); }
step s1dealloc	{ DEALLOCATE f; }
step s1c		{ COMMIT; }

session s2
step s2detach	{ ALTER TABLE d_listp DETACH PARTITION d_listp2 CONCURRENTLY; }
step s2drop		{ DROP TABLE d_listp2; }

session s3
step s3s		{ SELECT * FROM d_listp; }
step s3i		{ SELECT relpartbound IS NULL FROM pg_class where relname = 'd_listp2'; }
step s3ins2		{ INSERT INTO d_listp VALUES (2); }

# The transaction that detaches hangs until it sees any older transaction
# terminate, as does anybody else.
permutation s1b s1s s2detach s1s s1c s1s

# relpartbound remains set until s1 commits
# XXX this could be timing dependent :-(
permutation s1b s1s s2detach s1s s3s s3i s1c s3i s2drop s1s

# In read-committed mode, the partition disappears from view of concurrent
# transactions immediately.  But if a write lock is held, then the detach
# has to wait.
permutation s1b   s1s          s2detach s1ins s1s s1c
permutation s1b   s1s s1ins2 s2detach s1ins s1s s1c

# In repeatable-read mode, the partition remains visible until commit even
# if the to-be-detached partition is not locked for write.
permutation s1brr s1s          s2detach s1ins s1s s1c
permutation s1brr s1s          s2detach         s1s s1c

# Another process trying to acquire a write lock will be blocked behind the
# detacher
permutation s1b s1ins2 s2detach s3ins2 s1c

# a prepared query is not blocked
permutation s1brr s1prep s1s s2detach s1s s1exec1 s3s s1dealloc s1c
permutation s1brr s1prep s1exec2 s2detach s1s s1exec2 s3s s1c s1dealloc
permutation s1brr s1prep s1s s2detach s1s s1exec2 s1c s1dealloc
permutation s1brr s1prep s2detach s1s s1exec2 s1c s1dealloc
permutation s1brr s1prep1 s2detach s1s s1exec2 s1c s1dealloc
permutation s1brr s1prep2 s2detach s1s s1exec2 s1c s1dealloc
