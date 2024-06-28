# Test race conditions involving:
# - s1: VACUUM inplace-updating a pg_class row
# - s2: GRANT/REVOKE making pg_class rows dead
# - s3: "VACUUM pg_class" making dead rows LP_UNUSED; DDL reusing them

# Need GRANT to make a non-HOT update.  Otherwise, "VACUUM pg_class" would
# leave an LP_REDIRECT that persists.  To get non-HOT, make rels so the
# pg_class row for vactest.orig50 is on a filled page (assuming BLCKSZ=8192).
# Just to save on filesystem syscalls, use relkind=c for every other rel.
setup
{
	CREATE EXTENSION injection_points;
	CREATE SCHEMA vactest;
	CREATE FUNCTION vactest.mkrels(text, int, int) RETURNS void
		LANGUAGE plpgsql SET search_path = vactest AS $$
	DECLARE
		tname text;
	BEGIN
		FOR i in $2 .. $3 LOOP
			tname := $1 || i;
			EXECUTE FORMAT('CREATE TYPE ' || tname || ' AS ()');
			RAISE DEBUG '% at %', tname, ctid
				FROM pg_class WHERE oid = tname::regclass;
		END LOOP;
	END
	$$;
}
setup	{ VACUUM FULL pg_class;  -- reduce free space }
setup
{
	SELECT vactest.mkrels('orig', 1, 49);
	CREATE TABLE vactest.orig50 ();
	SELECT vactest.mkrels('orig', 51, 100);
}

# XXX DROP causes an assertion failure; adopt DROP once fixed
teardown
{
	--DROP SCHEMA vactest CASCADE;
	DO $$BEGIN EXECUTE 'ALTER SCHEMA vactest RENAME TO schema' || oid FROM pg_namespace where nspname = 'vactest'; END$$;
	DROP EXTENSION injection_points;
}

# Wait during inplace update, in a VACUUM of vactest.orig50.
session s1
setup	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('inplace-before-pin', 'wait');
}
step vac1	{ VACUUM vactest.orig50;  -- wait during inplace update }
# One bug scenario leaves two live pg_class tuples for vactest.orig50 and zero
# live tuples for one of the "intruder" rels.  REINDEX observes the duplicate.
step read1	{
	REINDEX TABLE pg_class;  -- look for duplicates
	SELECT reltuples = -1 AS reltuples_unknown
	FROM pg_class WHERE oid = 'vactest.orig50'::regclass;
}


# Transactional updates of the tuple vac1 is waiting to inplace-update.
session s2
step grant2		{ GRANT SELECT ON TABLE vactest.orig50 TO PUBLIC; }


# Non-blocking actions.
session s3
step vac3		{ VACUUM pg_class; }
# Reuse the lp that vac1 is waiting to change.  I've observed reuse at the 1st
# or 18th CREATE, so create excess.
step mkrels3	{
	SELECT vactest.mkrels('intruder', 1, 100);  -- repopulate LP_UNUSED
	SELECT injection_points_detach('inplace-before-pin');
	SELECT injection_points_wakeup('inplace-before-pin');
}


# XXX extant bug
permutation
	vac1(mkrels3)	# reads pg_class tuple T0 for vactest.orig50, xmax invalid
	grant2			# T0 becomes eligible for pruning, T1 is successor
	vac3			# T0 becomes LP_UNUSED
	mkrels3			# T0 reused; vac1 wakes and overwrites the reused T0
	read1
