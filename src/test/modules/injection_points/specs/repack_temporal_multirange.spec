# REPACK (CONCURRENTLY) on a temporal replica identity index with lossy
# multirange equality.
#
# The leading identity column is of type int4multirange. Two distinct rows
# have different multirange values but the same union range, so GiST equality
# can produce both as candidates and requires exact recheck.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_temporal_multirange (
		id int4multirange,
		valid_at datemultirange,
		label text,
		CONSTRAINT rtm_pkey PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
	);

	ALTER TABLE repack_temporal_multirange
		REPLICA IDENTITY USING INDEX rtm_pkey;

	-- (1,3)+(5,7) is the same union range of (1-7), but needs recheck
	INSERT INTO repack_temporal_multirange(id, valid_at, label)
	VALUES
		(int4multirange(int4range(1, 3), int4range(5, 7)),
		 datemultirange(daterange('2000-01-01', '2000-02-01')),
		 'other'),
		(int4multirange(int4range(1, 7)),
		 datemultirange(daterange('2000-01-01', '2000-02-01')),
		 'target');

	CREATE TABLE relfilenodes(node oid);
}

teardown
{
	DROP TABLE repack_temporal_multirange;
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
	REPACK (CONCURRENTLY) repack_temporal_multirange
		USING INDEX rtm_pkey;
}
step final_check
{
	INSERT INTO relfilenodes(node)
	SELECT relfilenode
	FROM pg_class
	WHERE relname = 'repack_temporal_multirange';

	-- Expect 2, proving that repack has rewritten the table
	SELECT count(DISTINCT node) FROM relfilenodes;

	-- Expect 2 rows
	SELECT id, valid_at, label
	FROM repack_temporal_multirange
	ORDER BY id, valid_at, label;
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2
step update_target
{
	UPDATE repack_temporal_multirange
	SET label = 'updated'
	WHERE id = int4multirange(int4range(1, 7))
	  AND valid_at = datemultirange(daterange('2000-01-01', '2000-02-01'));
}
step check_after_update
{
	INSERT INTO relfilenodes(node)
	SELECT relfilenode
	FROM pg_class
	WHERE relname = 'repack_temporal_multirange';

	-- Expect 2 rows
	SELECT id, valid_at, label
	FROM repack_temporal_multirange
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
	final_check
