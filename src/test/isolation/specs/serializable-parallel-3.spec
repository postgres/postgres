# Exercise the case where a read-only serializable transaction has
# SXACT_FLAG_RO_SAFE set in a parallel query.  This variant is like
# two copies of #2 running at the same time, and excercises the case
# where another transaction has the same xmin, and it is the oldest.

setup
{
	CREATE TABLE foo AS SELECT generate_series(1, 10)::int a;
	ALTER TABLE foo SET (parallel_workers = 2);
}

teardown
{
	DROP TABLE foo;
}

session s1
setup 		{ BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step s1r	{ SELECT * FROM foo; }
step s1c 	{ COMMIT; }

session s2
setup		{
			  BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE READ ONLY;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			}
step s2r1	{ SELECT * FROM foo; }
step s2r2	{ SELECT * FROM foo; }
step s2c	{ COMMIT; }

session s3
setup		{ BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step s3r	{ SELECT * FROM foo; }
step s3c	{ COMMIT; }

session s4
setup		{
			  BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE READ ONLY;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			}
step s4r1	{ SELECT * FROM foo; }
step s4r2	{ SELECT * FROM foo; }
step s4c	{ COMMIT; }

permutation s1r s3r s2r1 s4r1 s1c s2r2 s3c s4r2 s4c s2c
