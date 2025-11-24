# This test verifies INSERT ON CONFLICT DO UPDATE behavior concurrent with
# REINDEX CONCURRENTLY.
#
# - s1: UPSERT a tuple
# - s2: UPSERT the same tuple
# - s3: REINDEX concurrent primary key index
#
# - s4: controls concurrency via injection points

setup
{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA test;
	CREATE UNLOGGED TABLE test.tbl (i int PRIMARY KEY, updated_at timestamp);
	ALTER TABLE test.tbl SET (parallel_workers=0);
}

teardown
{
	DROP SCHEMA test CASCADE;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
}
step s1_start_upsert
{
	INSERT INTO test.tbl VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
}

session s2
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
}
step s2_start_upsert
{
	INSERT INTO test.tbl VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
}

session s3
setup
{
	SELECT injection_points_set_local();
}
step s3_setup_wait_before_set_dead
{
	SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
}
step s3_setup_wait_before_swap
{
	SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
}
step s3_start_reindex
{
	REINDEX INDEX CONCURRENTLY test.tbl_pkey;
}

session s4
step s4_wakeup_to_swap
{
	SELECT injection_points_detach('reindex-relation-concurrently-before-swap');
	SELECT injection_points_wakeup('reindex-relation-concurrently-before-swap');
}
step s4_wakeup_s1
{
	SELECT injection_points_detach('check-exclusion-or-unique-constraint-no-conflict');
	SELECT injection_points_wakeup('check-exclusion-or-unique-constraint-no-conflict');
}
step s4_wakeup_s2
{
	SELECT injection_points_detach('exec-insert-before-insert-speculative');
	SELECT injection_points_wakeup('exec-insert-before-insert-speculative');
}
step s4_wakeup_to_set_dead
{
	SELECT injection_points_detach('reindex-relation-concurrently-before-set-dead');
	SELECT injection_points_wakeup('reindex-relation-concurrently-before-set-dead');
}

permutation
	s3_setup_wait_before_set_dead
	s3_start_reindex(s1_start_upsert, s2_start_upsert)
	s1_start_upsert
	s4_wakeup_to_set_dead
	s2_start_upsert(s1_start_upsert)
	s4_wakeup_s1
	s4_wakeup_s2

permutation
	s3_setup_wait_before_swap
	s3_start_reindex(s1_start_upsert, s2_start_upsert)
	s1_start_upsert
	s4_wakeup_to_swap
	s2_start_upsert(s1_start_upsert)
	s4_wakeup_s2
	s4_wakeup_s1

permutation
	s3_setup_wait_before_set_dead
	s3_start_reindex(s1_start_upsert, s2_start_upsert)
	s1_start_upsert
	s2_start_upsert(s1_start_upsert)
	s4_wakeup_s1
	s4_wakeup_to_set_dead
	s4_wakeup_s2
