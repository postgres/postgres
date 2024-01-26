# This test exercises behavior of foreign keys in the face of concurrent
# detach of partitions in the referenced table.
# (The cases where the detaching transaction is canceled is interesting
# because the locking situation is completely different.  I didn't verify
# that keeping both variants adds any extra coverage.)
#
# Note: When using "s1cancel", mark the target step (the one to be canceled)
# as blocking "s1cancel".  This ensures consistent reporting regardless of
# whether "s1cancel" finishes before or after the other step reports failure.
# Also, ensure the step after "s1cancel" is also an s1 step (use "s1noop" if
# necessary).  This ensures we won't move on to the next step until the cancel
# is complete.

setup {
	drop table if exists d4_primary, d4_primary1, d4_fk, d4_pid;
	create table d4_primary (a int primary key) partition by list (a);
	create table d4_primary1 partition of d4_primary for values in (1);
	create table d4_primary2 partition of d4_primary for values in (2);
	insert into d4_primary values (1);
	insert into d4_primary values (2);
	create table d4_fk (a int references d4_primary);
	insert into d4_fk values (2);
	create table d4_pid (pid int);
}

session s1
step s1b			{ begin; }
step s1brr			{ begin isolation level repeatable read; }
step s1s			{ select * from d4_primary; }
step s1cancel 		{ select pg_cancel_backend(pid) from d4_pid; }
step s1noop			{ }
step s1insert		{ insert into d4_fk values (1); }
step s1c			{ commit; }
step s1declare		{ declare f cursor for select * from d4_primary; }
step s1declare2		{ declare f cursor for select * from d4_fk where a = 2; }
step s1fetchall		{ fetch all from f; }
step s1fetchone		{ fetch 1 from f; }
step s1updcur		{ update d4_fk set a = 1 where current of f; }
step s1svpt			{ savepoint f; }
step s1rollback		{ rollback to f; }

session s2
step s2snitch		{ insert into d4_pid select pg_backend_pid(); }
step s2detach		{ alter table d4_primary detach partition d4_primary1 concurrently; }

session s3
step s3brr			{ begin isolation level repeatable read; }
step s3insert		{ insert into d4_fk values (1); }
step s3commit		{ commit; }
step s3vacfreeze	{ vacuum freeze pg_catalog.pg_inherits; }

# Trying to insert into a partially detached partition is rejected
permutation s2snitch s1b   s1s s2detach s1cancel(s2detach) s1insert s1c
permutation s2snitch s1b   s1s s2detach                    s1insert s1c
# ... even under REPEATABLE READ mode.
permutation s2snitch s1brr s1s s2detach s1cancel(s2detach) s1insert s1c
permutation s2snitch s1brr s1s s2detach                    s1insert s1c

# If you read the referenced table using a cursor, you can see a row that the
# RI query does not see.
permutation s2snitch s1b s1declare s2detach s1cancel(s2detach) s1fetchall s1insert s1c
permutation s2snitch s1b s1declare s2detach                    s1fetchall s1insert s1c
permutation s2snitch s1b s1declare s2detach s1cancel(s2detach) s1svpt s1insert s1rollback s1fetchall s1c
permutation s2snitch s1b s1declare s2detach                    s1svpt s1insert s1rollback s1fetchall s1c
permutation s2snitch s1b s2detach s1declare s1cancel(s2detach) s1fetchall s1insert s1c
permutation s2snitch s1b s2detach s1declare                    s1fetchall s1insert s1c
permutation s2snitch s1b s2detach s1declare s1cancel(s2detach) s1svpt s1insert s1rollback s1fetchall s1c
permutation s2snitch s1b s2detach s1declare                    s1svpt s1insert s1rollback s1fetchall s1c

# Creating the referencing row using a cursor
permutation s2snitch s1brr s1declare2 s1fetchone s2detach s1cancel(s2detach) s1updcur s1c
permutation s2snitch s1brr s1declare2 s1fetchone s2detach                    s1updcur s1c
permutation s2snitch s1brr s1declare2 s1fetchone s1updcur s2detach s1c

# Try reading the table from an independent session.
permutation s2snitch s1b s1s s2detach s3insert s1c
permutation s2snitch s1b s1s s2detach s3brr s3insert s3commit s1cancel(s2detach) s1c
permutation s2snitch s1b s1s s2detach s3brr s3insert s3commit s1c

# Try one where we VACUUM FREEZE pg_inherits (to verify that xmin change is
# handled correctly).
permutation s2snitch s1brr s1s s2detach s1cancel(s2detach) s1noop s3vacfreeze s1s s1insert s1c
permutation s2snitch s1b s1s s2detach s1cancel(s2detach) s1noop s3vacfreeze s1s s1insert s1c
