# REPACK (CONCURRENTLY);
#
# Test handling of TOAST. At the same time, no tuplesort.
setup
{

	CREATE EXTENSION IF NOT EXISTS injection_points;

	-- Generate text consisting of repeated strings so that it can be
	-- compressed easily.
	CREATE FUNCTION gen_compressible(seed int)
	RETURNS text
	LANGUAGE sql IMMUTABLE as $$
		SELECT repeat(md5((seed * 1000)::text), 50);
	$$;

	-- Like above, but too big even after compression.
	CREATE FUNCTION gen_compressible_external(seed int)
	RETURNS text
	LANGUAGE sql IMMUTABLE as $$
		SELECT repeat(md5((seed * 1000)::text), 10000);
	$$;

	-- Generate a string of random characters that is not likely to be
	-- compressed, but is big enough to be stored externally.
	CREATE FUNCTION gen_external()
	RETURNS text
	LANGUAGE sql as $$
		SELECT string_agg(chr(65 + trunc(25 * random())::int), '')
		FROM generate_series(1, 2048) s(x);
	$$;

	-- Not compressible like above, but small enough to stay in-line.
	CREATE FUNCTION gen_inline()
	RETURNS text
	LANGUAGE sql as $$
		SELECT string_agg(chr(65 + trunc(25 * random())::int), '')
		FROM generate_series(1, 1024) s(x);
	$$;

	-- A varlena short enough to have a one-byte header.
	CREATE FUNCTION gen_short()
	RETURNS text
	LANGUAGE sql as $$
		SELECT string_agg(chr(65 + trunc(25 * random())::int), '')
		FROM generate_series(1, 120) s(x);
	$$;

	CREATE TABLE repack_toast(drop1 int, i int PRIMARY KEY, drop2 int,
		j text COMPRESSION pglz, k text COMPRESSION pglz);
	INSERT INTO repack_toast(drop1, i, drop2, j, k)
	SELECT 42, gs, 42, gen_external(), gen_compressible(gs) FROM generate_series(1, 10) gs;
	ALTER TABLE repack_toast DROP COLUMN drop1, DROP COLUMN drop2;
	ALTER TABLE repack_toast ALTER COLUMN k SET COMPRESSION default;
	INSERT INTO repack_toast(i, j, k)
	SELECT gs, gen_external(), gen_compressible(142857) FROM generate_series(11, 20) gs;

	ALTER TABLE repack_toast SET (toast_tuple_target = 128);

	CREATE TABLE relfilenodes(node oid);

	CREATE TABLE data_s1 (i int, j text, j_toast oid, k text, k_toast oid);
	CREATE TABLE data_s2 (LIKE data_s1);
}


teardown
{
	DROP TABLE repack_toast;
	DROP EXTENSION injection_points;
	DROP FUNCTION gen_compressible(int);
	DROP FUNCTION gen_compressible_external(int);
	DROP FUNCTION gen_external();
	DROP FUNCTION gen_inline();
	DROP FUNCTION gen_short();

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
step s1_wait_before_lock
{
	REPACK (CONCURRENTLY) repack_toast;
}

# Check the table, after REPACK has completed.  s2 must have saved the data
# as it was visible to it.  We check that the relfilenode changed in addition
# to verifying that the actual data matches.
step s1_check
{
	INSERT INTO relfilenodes(node)
	SELECT c2.relfilenode
	FROM pg_class c1 JOIN pg_class c2 ON c2.oid = c1.oid OR c2.oid = c1.reltoastrelid
	WHERE c1.relname='repack_toast';

	SELECT count(DISTINCT node) FROM relfilenodes;

	INSERT INTO data_s1(i, j, j_toast, k, k_toast)
	SELECT i,
	j, COALESCE(pg_column_toast_chunk_id(j), 0) AS j_toast,
	k, COALESCE(pg_column_toast_chunk_id(k), 0) AS k_toast
	FROM repack_toast;

	-- this should be empty
	SELECT d1.i, substring(d1.j FOR 12) AS d1_j, substring(d1.k FOR 12) AS d1_k,
		d2.i, substring(d2.j FOR 12) AS d2_j, substring(d2.k FOR 12) AS d2_k,
		d1.j_toast as d1_j_tst, d2.j_toast as d2_j_tst,
		d1.k_toast as d1_k_tst, d2.k_toast AS d2_k_tst
	FROM data_s1 d1 FULL JOIN data_s2 d2 USING (i, j, k)
	WHERE d1.i ISNULL OR d2.i ISNULL;
}
teardown
{
    SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2

# Test different kinds of toast data changes.
step s2_updates
{
	DELETE FROM repack_toast WHERE i=1;
	INSERT INTO repack_toast(i, j, k) VALUES (1, gen_external(), gen_compressible(1));

	-- existing toast data unchanged.  (This covers the case where we
	-- adjust the toast pointer.)
	UPDATE repack_toast SET i=i+300 where i % 10 = 2 RETURNING OLD.i, NEW.i;

	-- "j" is here an external indirect, written to the file separately.
	UPDATE repack_toast SET j=gen_external() where i % 10 = 3 RETURNING OLD.i, NEW.i;

	-- the updated value of "j" is compressed.
	UPDATE repack_toast SET j=gen_compressible(1), k=k||'' where i % 10 = 4 RETURNING i;

	-- the updated value of "j" is compressed externally.
	UPDATE repack_toast SET j=gen_compressible_external(2) where i % 10 = 5 RETURNING i;

	-- the updated value of "j" stays inline.
	UPDATE repack_toast SET j=gen_inline(), k=repeat(k,5) where i % 10 = 6 RETURNING i;

	-- updated value of "j" is a short varlena; "k" is written separately.
	UPDATE repack_toast SET j=gen_short(), k=gen_external() where i % 10 = 7 RETURNING i;
}

# Check the table from the perspective of s2.  This saves data so that it can
# be verified later.
step s2_check
{
	INSERT INTO relfilenodes(node)
	SELECT c2.relfilenode
	FROM pg_class c1 JOIN pg_class c2 ON c2.oid = c1.oid OR c2.oid = c1.reltoastrelid
	WHERE c1.relname='repack_toast';

	INSERT INTO data_s2(i, j, j_toast, k, k_toast)
	SELECT i, j, COALESCE(pg_column_toast_chunk_id(j), 0) AS j_toast,
	k, COALESCE(pg_column_toast_chunk_id(k), 0) AS k_toast
	FROM repack_toast;
}
step s2_wakeup_before_lock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

# Test if data changes introduced while one session is performing REPACK
# CONCURRENTLY find their way into the table.
permutation
	s1_wait_before_lock
	s2_updates
	s2_check
	s2_wakeup_before_lock
	s1_check
