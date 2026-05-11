# REPACK (CONCURRENTLY) on a temporal replica identity index.
#
# The table's replica identity is a GiST index created by a temporal primary
# key.  A concurrent UPDATE changes a non-key column of one row, while another
# row overlaps it on all indexed columns.  Replay must still find the exact
# target row.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_temporal (
		id int4range,
		valid_at daterange,
		label text,
		CONSTRAINT rt_pkey PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
	);

	ALTER TABLE repack_temporal REPLICA IDENTITY USING INDEX rt_pkey;

	INSERT INTO repack_temporal(id, valid_at, label)
	VALUES
		('[1,10)', '[2000-01-01,2000-02-01)', 'other'),
		('[2,3)',  '[2000-01-10,2000-01-20)', 'target');

	CREATE TABLE relfilenodes(node oid);
}

teardown
{
	DROP TABLE repack_temporal;
	DROP EXTENSION injection_points;
	DROP TABLE relfilenodes;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
step wait_before_lock
{
	REPACK (CONCURRENTLY) repack_temporal USING INDEX rt_pkey;
}
step check_after_repack
{
	INSERT INTO relfilenodes(node)
	SELECT relfilenode FROM pg_class WHERE relname = 'repack_temporal';

	-- Expect 2, proving that repack has rewritten the table
	SELECT count(DISTINCT node) FROM relfilenodes;

	-- Expect 2 rows
	SELECT id, valid_at, label
	FROM repack_temporal
	ORDER BY id, valid_at, label;
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2
step update_target
{
	UPDATE repack_temporal
	SET label = 'updated'
	WHERE id = '[2,3)' AND valid_at = '[2000-01-10,2000-01-20)';
}
step check_after_update
{
	INSERT INTO relfilenodes(node)
	SELECT relfilenode FROM pg_class WHERE relname = 'repack_temporal';

	-- Expect 2 rows
	SELECT id, valid_at, label
	FROM repack_temporal
	ORDER BY id, valid_at, label;
}
step wakeup_before_lock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation
	wait_before_lock
	update_target
	check_after_update
	wakeup_before_lock
	check_after_repack
