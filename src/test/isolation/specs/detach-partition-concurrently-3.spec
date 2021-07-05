# Try various things to happen to a partition with an incomplete detach
#
# Note: When using "s1cancel", mark the target step (the one to be canceled)
# as blocking "s1cancel".  This ensures consistent reporting regardless of
# whether "s1cancel" finishes before or after the other step reports failure.
# Also, ensure the step after "s1cancel" is also an s1 step (use "s1noop" if
# necessary).  This ensures we won't move on to the next step until the cancel
# is complete.

setup
{
  CREATE TABLE d3_listp (a int) PARTITION BY LIST(a);
  CREATE TABLE d3_listp1 PARTITION OF d3_listp FOR VALUES IN (1);
  CREATE TABLE d3_listp2 PARTITION OF d3_listp FOR VALUES IN (2);
  CREATE TABLE d3_pid (pid int);
  INSERT INTO d3_listp VALUES (1);
}

teardown {
    DROP TABLE IF EXISTS d3_listp, d3_listp1, d3_listp2, d3_pid;
}

session s1
step s1b			{ BEGIN; }
step s1brr			{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1s			{ SELECT * FROM d3_listp; }
step s1spart		{ SELECT * FROM d3_listp1; }
step s1cancel 		{ SELECT pg_cancel_backend(pid) FROM d3_pid; }
step s1noop			{ }
step s1c			{ COMMIT; }
step s1alter		{ ALTER TABLE d3_listp1 ALTER a DROP NOT NULL; }
step s1insert		{ INSERT INTO d3_listp VALUES (1); }
step s1insertpart	{ INSERT INTO d3_listp1 VALUES (1); }
step s1drop			{ DROP TABLE d3_listp; }
step s1droppart		{ DROP TABLE d3_listp1; }
step s1trunc		{ TRUNCATE TABLE d3_listp; }
step s1list			{ SELECT relname FROM pg_catalog.pg_class
					  WHERE relname LIKE 'd3_listp%' ORDER BY 1; }
step s1describe		{ SELECT 'd3_listp' AS root, * FROM pg_partition_tree('d3_listp')
					  UNION ALL SELECT 'd3_listp1', * FROM pg_partition_tree('d3_listp1'); }

session s2
step s2begin		{ BEGIN; }
step s2snitch		{ INSERT INTO d3_pid SELECT pg_backend_pid(); }
step s2detach		{ ALTER TABLE d3_listp DETACH PARTITION d3_listp1 CONCURRENTLY; }
step s2detach2		{ ALTER TABLE d3_listp DETACH PARTITION d3_listp2 CONCURRENTLY; }
step s2detachfinal	{ ALTER TABLE d3_listp DETACH PARTITION d3_listp1 FINALIZE; }
step s2drop			{ DROP TABLE d3_listp1; }
step s2commit		{ COMMIT; }

# Try various things while the partition is in "being detached" state, with
# no session waiting.
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1describe s1alter
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1insert s1c
permutation s2snitch s1brr s1s s2detach s1cancel(s2detach) s1insert s1c s1spart
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1insertpart

# Test partition descriptor caching
permutation s2snitch s1b s1s s2detach2 s1cancel(s2detach2) s1c s1brr s1insert s1s s1insert s1c
permutation s2snitch s1b s1s s2detach2 s1cancel(s2detach2) s1c s1brr s1s s1insert s1s s1c

# "drop" here does both tables
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1drop s1list
# "truncate" only does parent, not partition
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1trunc s1spart

# If a partition pending detach exists, we cannot drop another one
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1noop s2detach2 s1c
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1noop s2detachfinal s1c s2detach2
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1droppart s2detach2

# When a partition with incomplete detach is dropped, we grab lock on parent too.
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s2begin s2drop s1s s2commit

# Partially detach, then select and try to complete the detach.  Reading
# from partition blocks (AEL is required on partition); reading from parent
# does not block.
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1b s1spart s2detachfinal s1c
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1b s1s s2detachfinal s1c

# DETACH FINALIZE in a transaction block. No insert/select on the partition
# is allowed concurrently with that.
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s1b s1spart s2detachfinal s1c
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s2begin s2detachfinal s2commit
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s2begin s2detachfinal s1spart s2commit
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1c s2begin s2detachfinal s1insertpart s2commit
