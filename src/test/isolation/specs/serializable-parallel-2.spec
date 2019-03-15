# Exercise the case where a read-only serializable transaction has
# SXACT_FLAG_RO_SAFE set in a parallel query.

setup
{
	CREATE TABLE foo AS SELECT generate_series(1, 10)::int a;
	ALTER TABLE foo SET (parallel_workers = 2);
}

teardown
{
	DROP TABLE foo;
}

session "s1"
setup 		{ BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step "s1r"	{ SELECT * FROM foo; }
step "s1c" 	{ COMMIT; }

session "s2"
setup		{
			  BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE READ ONLY;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			}
step "s2r1"	{ SELECT * FROM foo; }
step "s2r2"	{ SELECT * FROM foo; }
step "s2c"	{ COMMIT; }

permutation "s1r" "s2r1" "s1c" "s2r2" "s2c"
