setup
{
	CREATE EXTENSION injection_points;

	BEGIN;
	-- Generate a string of random characters that is not likely to be
	-- compressed, but is big enough to be stored externally.
	CREATE FUNCTION gen_external()
	RETURNS text
	LANGUAGE sql as $$
		SELECT string_agg(chr(65 + trunc(25 * random())::int), '')
		FROM generate_series(1, 2048) s(x);
	$$;
	COMMIT;

	SELECT pg_create_logical_replication_slot('s', 'test_decoding');

	CREATE TABLE repack_toast(i int PRIMARY KEY, t text);
	INSERT INTO repack_toast(i, t) VALUES (1, gen_external());
}

teardown
{
	DROP TABLE repack_toast;
	DROP EXTENSION injection_points;
	DROP FUNCTION gen_external();
	SELECT pg_drop_replication_slot('s');
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
# Perform the initial load and wait for s2 to do some data changes.
step s1_wait_before_lock
{
	REPACK (CONCURRENTLY) repack_toast;
}
step s1_decode
{
	SELECT count(*) FROM pg_logical_slot_peek_changes('s', NULL, NULL, 'include-rewrites', '1');
}
# Show the decoded updates.  The row loaded by setup carries a random TOAST
# value, so only the updates are stable enough to display.
step s1_decode_updates
{
	SELECT data FROM pg_logical_slot_peek_changes('s', NULL, NULL, 'include-rewrites', '1')
	WHERE data LIKE '%UPDATE%';
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2
step s2_changes
{
	UPDATE repack_toast SET t = gen_external() WHERE i=1;
}
# Change the row using a value small enough to stay in-line.
step s2_short_change
{
	UPDATE repack_toast SET t = 'short' WHERE i=1;
}
step s2_wakeup_before_lock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation
	s1_wait_before_lock
	s2_changes
	s2_wakeup_before_lock
	s1_decode

# The changes REPACK applies to the transient heap must not be reported to an
# output plugin, not even as records that carry no tuple.  Unlike the
# permutation above, no TOAST value is involved here.
permutation
	s1_wait_before_lock
	s2_short_change
	s2_wakeup_before_lock
	s1_decode_updates
