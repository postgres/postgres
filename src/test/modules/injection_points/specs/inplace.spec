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
teardown
{
	DROP SCHEMA vactest CASCADE;
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
step revoke2	{ REVOKE SELECT ON TABLE vactest.orig50 FROM PUBLIC; }
step begin2		{ BEGIN; }
step c2			{ COMMIT; }
step r2			{ ROLLBACK; }

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


# target gains a successor at the last moment
permutation
	vac1(mkrels3)	# reads pg_class tuple T0 for vactest.orig50, xmax invalid
	grant2			# T0 becomes eligible for pruning, T1 is successor
	vac3			# T0 becomes LP_UNUSED
	mkrels3			# vac1 wakes, scans to T1
	read1

# target already has a successor, which commits
permutation
	begin2
	grant2			# T0.t_ctid = T1
	vac1(mkrels3)	# reads T0 for vactest.orig50
	c2				# T0 becomes eligible for pruning
	vac3			# T0 becomes LP_UNUSED
	mkrels3			# vac1 wakes, scans to T1
	read1

# target already has a successor, which becomes LP_UNUSED at the last moment
permutation
	begin2
	grant2			# T0.t_ctid = T1
	vac1(mkrels3)	# reads T0 for vactest.orig50
	r2				# T1 becomes eligible for pruning
	vac3			# T1 becomes LP_UNUSED
	mkrels3			# reuse T1; vac1 scans to T0
	read1

# target already has a successor, which becomes LP_REDIRECT at the last moment
permutation
	begin2
	grant2			# T0.t_ctid = T1, non-HOT due to filled page
	vac1(mkrels3)	# reads T0
	c2
	revoke2			# HOT update to T2
	grant2			# HOT update to T3
	vac3			# T1 becomes LP_REDIRECT
	mkrels3			# reuse T2; vac1 scans to T3
	read1

# waiting for updater to end
permutation
	vac1(c2)		# reads pg_class tuple T0 for vactest.orig50, xmax invalid
	begin2
	grant2			# T0.t_ctid = T1, non-HOT due to filled page
	revoke2			# HOT update to T2
	mkrels3			# vac1 awakes briefly, then waits for s2
	c2
	read1

# Another LP_UNUSED.  This time, do change the live tuple.  Final live tuple
# body is identical to original, at a different TID.
permutation
	begin2
	grant2			# T0.t_ctid = T1, non-HOT due to filled page
	vac1(mkrels3)	# reads T0
	r2				# T1 becomes eligible for pruning
	grant2			# T0.t_ctid = T2; T0 becomes eligible for pruning
	revoke2			# T2.t_ctid = T3; T2 becomes eligible for pruning
	vac3			# T0, T1 & T2 become LP_UNUSED
	mkrels3			# reuse T0, T1 & T2; vac1 scans to T3
	read1

# Another LP_REDIRECT.  Compared to the earlier test, omit the last grant2.
# Hence, final live tuple body is identical to original, at a different TID.
permutation begin2 grant2 vac1(mkrels3) c2 revoke2 vac3 mkrels3 read1
