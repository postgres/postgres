# Test REPACK (CONCURRENTLY) removing values from dropped columns.
#
# The tuples are large enough to use two disk blocks; after dropping column b,
# they'll comfortably fit in a single page.  The OLD tuple 1 is propagated
# through the concurrent update because of the trigger.
setup {
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_dropped (id int PRIMARY KEY, a text, b text);
	ALTER TABLE repack_dropped ALTER COLUMN b SET STORAGE EXTERNAL;
	INSERT INTO repack_dropped (id, a, b) VALUES (1, 'one',
		repeat(encode(sha256('1'), 'hex'), current_setting('block_size')::int / 32));
	INSERT INTO repack_dropped (id, a, b) VALUES (2, 'two',
		repeat(encode(sha256('2'), 'hex'), current_setting('block_size')::int / 32));
	CREATE FUNCTION repack_dropped_f() RETURNS trigger LANGUAGE plpgsql AS
		$$ DECLARE r repack_dropped;
		BEGIN
			IF TG_OP = 'INSERT' THEN
				r := (SELECT t FROM repack_dropped t WHERE id = 1);
				r.id := NEW.id;
				RETURN r;
			END IF;
			IF NEW.id = 1 THEN return OLD; END IF;
			RETURN NEW;
		END $$;
	CREATE TRIGGER repack_dropped_t BEFORE UPDATE ON repack_dropped FOR EACH ROW EXECUTE FUNCTION repack_dropped_f();
	CREATE TRIGGER repack_dropped_ins_t BEFORE INSERT ON repack_dropped FOR EACH ROW EXECUTE FUNCTION repack_dropped_f();
	ALTER TABLE repack_dropped DROP COLUMN b;
}

teardown {
	DROP TABLE repack_dropped;
	DROP FUNCTION repack_dropped_f;
	DROP EXTENSION injection_points;
}

session s1

step s1_size
{
	SELECT id, pg_column_size(repack_dropped) < current_setting('block_size')::int
		AS fits_in_one_block
		FROM repack_dropped ORDER BY id;
}

step s1_unlock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

session s2
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}

step s2_repack
{
	REPACK (CONCURRENTLY) repack_dropped;
}

step s2_noop
{
}

session s3
step s3_updates
{
	UPDATE repack_dropped SET a = a || a;
}

# The trigger makes tuple 3 a copy of tuple 1, dropped column included.
step s3_insert
{
	INSERT INTO repack_dropped (id, a) VALUES (3, 'three');
}

permutation
	s1_size
	s2_repack
	s3_updates
	s3_insert
	s1_unlock
	s2_noop
	s1_size
