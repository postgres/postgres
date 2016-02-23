# This is a straightforward deadlock scenario.  Since it involves more than
# two processes, the main lock detector will find the problem and rollback
# the session that first discovers it.  Set deadlock_timeout in each session
# so that it's predictable which session fails.

setup
{
  CREATE TABLE a1 ();
  CREATE TABLE a2 ();
  CREATE TABLE a3 ();
  CREATE TABLE a4 ();
  CREATE TABLE a5 ();
  CREATE TABLE a6 ();
  CREATE TABLE a7 ();
  CREATE TABLE a8 ();
}

teardown
{
  DROP TABLE a1, a2, a3, a4, a5, a6, a7, a8;
}

session "s1"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s1a1"	{ LOCK TABLE a1; }
step "s1a2"	{ LOCK TABLE a2; }
step "s1c"	{ COMMIT; }

session "s2"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s2a2"	{ LOCK TABLE a2; }
step "s2a3"	{ LOCK TABLE a3; }
step "s2c"	{ COMMIT; }

session "s3"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s3a3"	{ LOCK TABLE a3; }
step "s3a4"	{ LOCK TABLE a4; }
step "s3c"	{ COMMIT; }

session "s4"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s4a4"	{ LOCK TABLE a4; }
step "s4a5"	{ LOCK TABLE a5; }
step "s4c"	{ COMMIT; }

session "s5"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s5a5"	{ LOCK TABLE a5; }
step "s5a6"	{ LOCK TABLE a6; }
step "s5c"	{ COMMIT; }

session "s6"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s6a6"	{ LOCK TABLE a6; }
step "s6a7"	{ LOCK TABLE a7; }
step "s6c"	{ COMMIT; }

session "s7"
setup		{ BEGIN; SET deadlock_timeout = '100s'; }
step "s7a7"	{ LOCK TABLE a7; }
step "s7a8"	{ LOCK TABLE a8; }
step "s7c"	{ COMMIT; }

session "s8"
setup		{ BEGIN; SET deadlock_timeout = '10s'; }
step "s8a8"	{ LOCK TABLE a8; }
step "s8a1"	{ LOCK TABLE a1; }
step "s8c"	{ COMMIT; }

permutation "s1a1" "s2a2" "s3a3" "s4a4" "s5a5" "s6a6" "s7a7" "s8a8" "s1a2" "s2a3" "s3a4" "s4a5" "s5a6" "s6a7" "s7a8" "s8a1" "s8c" "s7c" "s6c" "s5c" "s4c" "s3c" "s2c" "s1c"
