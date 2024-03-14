# While requesting nowait lock, if the lock requested should
# be inserted in front of some waiter, check to see if the lock
# conflicts with already-held locks or the requests before
# the waiter. If not, then just grant myself the requested
# lock immediately.  Test this scenario.

setup
{
  CREATE TABLE a1 ();
}

teardown
{
  DROP TABLE a1;
}

session s1
setup		{ BEGIN; }
step s1a	{ LOCK TABLE a1 IN ACCESS EXCLUSIVE MODE; }
step s1b	{ LOCK TABLE a1 IN SHARE ROW EXCLUSIVE MODE NOWAIT; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2a	{ LOCK TABLE a1 IN EXCLUSIVE MODE; }
step s2c	{ COMMIT; }

permutation s1a s2a s1b s1c s2c
