# This test verifies INSERT ON CONFLICT DO UPDATE behavior concurrent with
# CREATE INDEX CONCURRENTLY.
#
# - s1: UPSERT a tuple
# - s2: UPSERT the same tuple
# - s3: CREATE UNIQUE INDEX CONCURRENTLY
#
# - s4: Control concurrency using injection points

setup
{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA test;
	CREATE UNLOGGED TABLE test.tbl(i int primary key, updated_at timestamp);
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
	SELECT injection_points_attach('invalidate-catalog-snapshot-end', 'wait');
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
	SELECT injection_points_attach('define-index-before-set-valid', 'wait');
}
step s3_start_create_index
{
	CREATE UNIQUE INDEX CONCURRENTLY tbl_pkey_duplicate ON test.tbl(i);
}

session s4
step s4_wakeup_s1
{
	SELECT injection_points_detach('check-exclusion-or-unique-constraint-no-conflict');
	SELECT injection_points_wakeup('check-exclusion-or-unique-constraint-no-conflict');
}
step s4_wakeup_s1_from_invalidate_catalog_snapshot
{
	SELECT injection_points_detach('invalidate-catalog-snapshot-end');
	SELECT injection_points_wakeup('invalidate-catalog-snapshot-end');
}
step s4_wakeup_s2
{
	SELECT injection_points_detach('exec-insert-before-insert-speculative');
	SELECT injection_points_wakeup('exec-insert-before-insert-speculative');
}
step s4_wakeup_define_index_before_set_valid
{
	SELECT injection_points_detach('define-index-before-set-valid');
	SELECT injection_points_wakeup('define-index-before-set-valid');
}

permutation
	s3_start_create_index(s1_start_upsert, s2_start_upsert)
	s1_start_upsert
	s4_wakeup_define_index_before_set_valid
	s2_start_upsert(s1_start_upsert)
	s4_wakeup_s1_from_invalidate_catalog_snapshot
	s4_wakeup_s2
	s4_wakeup_s1
