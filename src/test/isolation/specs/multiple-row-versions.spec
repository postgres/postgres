# Multiple Row Versions test
#
# This test is designed to cover some code paths which only occur with
# four or more transactions interacting with particular timings.
#
# Due to long permutation setup time, we are only testing one specific
# permutation, which should get a serialization error.

setup
{
 CREATE TABLE t (id int NOT NULL, txt text) WITH (fillfactor=50);
 INSERT INTO t (id)
   SELECT x FROM (SELECT * FROM generate_series(1, 1000000)) a(x);
 ALTER TABLE t ADD PRIMARY KEY (id);
}

teardown
{
 DROP TABLE t;
}

session "s1"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rx1"	{ SELECT * FROM t WHERE id = 1000000; }
# delay until after T3 commits
step "wz1"	{ UPDATE t SET txt = 'a' WHERE id = 1; }
step "c1"	{ COMMIT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "wx2"	{ UPDATE t SET txt = 'b' WHERE id = 1000000; }
step "c2"	{ COMMIT; }

session "s3"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "wx3"	{ UPDATE t SET txt = 'c' WHERE id = 1000000; }
step "ry3"	{ SELECT * FROM t WHERE id = 500000; }
# delay until after T4 commits
step "c3"	{ COMMIT; }

session "s4"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "wy4"	{ UPDATE t SET txt = 'd' WHERE id = 500000; }
step "rz4"	{ SELECT * FROM t WHERE id = 1; }
step "c4"	{ COMMIT; }

permutation "rx1" "wx2" "c2" "wx3" "ry3" "wy4" "rz4" "c4" "c3" "wz1" "c1"
