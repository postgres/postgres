# Test interaction of REPACK (CONCURRENTLY) with an ALTER TABLE .. ADD COLUMN
# that doesn't rewrite the table (pg_attribute.attmissingval).
setup
{
	CREATE EXTENSION IF NOT EXISTS injection_points;

	CREATE TABLE repack_missingval(i int PRIMARY KEY, j int);
	INSERT INTO repack_missingval(i, j) VALUES (1, 1), (2, 2), (3, 3);

	-- Add a column with default values and don't rewrite the table; use it
	-- in the replica identity also.
	ALTER TABLE repack_missingval ADD COLUMN c text NOT NULL DEFAULT 'xyz';
	CREATE UNIQUE INDEX repack_missingval_idx ON repack_missingval (i, c);
	ALTER TABLE repack_missingval REPLICA IDENTITY USING INDEX repack_missingval_idx;

	-- Set up a trigger on UPDATE that returns the original tuple.  This way,
	-- the original short tuple is given to logical decoding.
	CREATE FUNCTION repack_return_old() RETURNS trigger
	LANGUAGE plpgsql AS $$
	BEGIN
		RETURN OLD;
	END;
	$$;
	CREATE TRIGGER return_old BEFORE UPDATE ON repack_missingval
	FOR EACH ROW EXECUTE FUNCTION repack_return_old();
}

teardown
{
	DROP TABLE repack_missingval;
	DROP FUNCTION repack_return_old();
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}

# Perform the initial load and wait for s2 to change the data.
step s1_wait_before_lock
{
	REPACK (CONCURRENTLY) repack_missingval;
}

# The missing values must have survived the concurrent changes.
step s1_check
{
	SELECT i, j, c FROM repack_missingval ORDER BY i;
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2

# Update two of the three rows.
step s2_update
{
	UPDATE repack_missingval SET j = j WHERE i IN (1, 2);
}
step s2_wakeup_before_lock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation
	s1_wait_before_lock
	s2_update
	s2_wakeup_before_lock
	s1_check
