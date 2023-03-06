# Exercise the case where a read-only serializable transaction has
# SXACT_FLAG_RO_SAFE set in a parallel query.

setup
{
	CREATE TABLE foo AS SELECT generate_series(1, 100)::int a;
	CREATE INDEX ON foo(a);
	ALTER TABLE foo SET (parallel_workers = 2);
}

teardown
{
	DROP TABLE foo;
}

session s1
setup 		{ BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; }
step s1r	{ SELECT COUNT(*) FROM foo; }
step s1c 	{ COMMIT; }

session s2
setup		{
			  BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE READ ONLY;
			  SET parallel_setup_cost = 0;
			  SET parallel_tuple_cost = 0;
			  SET min_parallel_index_scan_size = 0;
			  SET parallel_leader_participation = off;
			  SET enable_seqscan = off;
			}
step s2r1	{ SELECT COUNT(*) FROM foo; }
step s2r2	{ SELECT COUNT(*) FROM foo; }
step s2c	{ COMMIT; }

permutation s1r s2r1 s1c s2r2 s2c
