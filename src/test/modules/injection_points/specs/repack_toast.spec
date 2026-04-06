# REPACK (CONCURRENTLY);
#
# Test handling of TOAST. At the same time, no tuplesort.
setup
{
	CREATE EXTENSION injection_points;

	-- Return a string that needs to be TOASTed.
	CREATE FUNCTION get_long_string()
	RETURNS text
	LANGUAGE sql as $$
		SELECT string_agg(chr(65 + trunc(25 * random())::int), '')
		FROM generate_series(1, 2048) s(x);
	$$;

	CREATE TABLE repack_test(i int PRIMARY KEY, j text);
	INSERT INTO repack_test(i, j) VALUES (1, get_long_string()),
		(2, get_long_string()), (3, get_long_string());

	CREATE TABLE relfilenodes(node oid);

	CREATE TABLE data_s1(i int, j text);
	CREATE TABLE data_s2(i int, j text);
}

teardown
{
	DROP TABLE repack_test;
	DROP EXTENSION injection_points;
	DROP FUNCTION get_long_string();

	DROP TABLE relfilenodes;
	DROP TABLE data_s1;
	DROP TABLE data_s2;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
# Perform the initial load and wait for s2 to do some data changes.
step wait_before_lock
{
	REPACK (CONCURRENTLY) repack_test;
}
# Check the table from the perspective of s1.
#
# Besides the contents, we also check that relfilenode has changed.

# Have each session write the contents into a table and use FULL JOIN to check
# if the outputs are identical.
step check1
{
	INSERT INTO relfilenodes(node)
	SELECT c2.relfilenode
	FROM pg_class c1 JOIN pg_class c2 ON c2.oid = c1.oid OR c2.oid = c1.reltoastrelid
	WHERE c1.relname='repack_test';

	SELECT count(DISTINCT node) FROM relfilenodes;

	INSERT INTO data_s1(i, j)
	SELECT i, j FROM repack_test;

	SELECT count(*)
	FROM data_s1 d1 FULL JOIN data_s2 d2 USING (i, j)
	WHERE d1.i ISNULL OR d2.i ISNULL;
}
teardown
{
    SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2
step change
# Separately test UPDATE of both plain ("i") and TOASTed ("j") attribute. In
# the first case, the new tuple we get from reorderbuffer.c contains "j" as a
# TOAST pointer, which we need to update so it points to the new heap. In the
# latter case, we receive "j" as "external indirect" value - here we test that
# the decoding worker writes the tuple to a file correctly and that the
# backend executing REPACK manages to restore it.
{
	UPDATE repack_test SET j=get_long_string() where i=2;
	DELETE FROM repack_test WHERE i=3;
	INSERT INTO repack_test(i, j) VALUES (4, get_long_string());
	UPDATE repack_test SET i=3 where i=1;
}
# Check the table from the perspective of s2.
step check2
{
	INSERT INTO relfilenodes(node)
	SELECT c2.relfilenode
	FROM pg_class c1 JOIN pg_class c2 ON c2.oid = c1.oid OR c2.oid = c1.reltoastrelid
	WHERE c1.relname='repack_test';

	INSERT INTO data_s2(i, j)
	SELECT i, j FROM repack_test;
}
step wakeup_before_lock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

# Test if data changes introduced while one session is performing REPACK
# CONCURRENTLY find their way into the table.
permutation
	wait_before_lock
	change
	check2
	wakeup_before_lock
	check1
