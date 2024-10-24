# GRANT's lock is the catalog tuple xmax.  GRANT doesn't acquire a heavyweight
# lock on the object undergoing an ACL change.  Inplace updates, such as
# relhasindex=true, need special code to cope.

setup
{
	CREATE TABLE intra_grant_inplace (c int);
}

teardown
{
	DROP TABLE IF EXISTS intra_grant_inplace;
}

# heap_update()
session s1
setup	{ SET deadlock_timeout = '100s'; }
step b1	{ BEGIN; }
step grant1	{
	GRANT SELECT ON intra_grant_inplace TO PUBLIC;
}
step drop1	{
	DROP TABLE intra_grant_inplace;
}
step c1	{ COMMIT; }

# inplace update
session s2
setup	{ SET deadlock_timeout = '10ms'; }
step read2	{
	SELECT relhasindex FROM pg_class
	WHERE oid = 'intra_grant_inplace'::regclass;
}
step b2		{ BEGIN; }
step addk2	{ ALTER TABLE intra_grant_inplace ADD PRIMARY KEY (c); }
step sfnku2	{
	SELECT relhasindex FROM pg_class
	WHERE oid = 'intra_grant_inplace'::regclass FOR NO KEY UPDATE;
}
step c2		{ COMMIT; }

# rowmarks
session s3
step b3		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step sfnku3	{
	SELECT relhasindex FROM pg_class
	WHERE oid = 'intra_grant_inplace'::regclass FOR NO KEY UPDATE;
}
step sfu3	{
	SELECT relhasindex FROM pg_class
	WHERE oid = 'intra_grant_inplace'::regclass FOR UPDATE;
}
step r3	{ ROLLBACK; }

# Additional heap_update()
session s4
# swallow error message to keep any OID value out of expected output
step revoke4	{
	DO $$
	BEGIN
		REVOKE SELECT ON intra_grant_inplace FROM PUBLIC;
	EXCEPTION WHEN others THEN
		RAISE WARNING 'got: %', regexp_replace(sqlerrm, '[0-9]+', 'REDACTED');
	END
	$$;
}

# Additional rowmarks
session s5
setup	{ BEGIN; }
step keyshr5	{
	SELECT relhasindex FROM pg_class
	WHERE oid = 'intra_grant_inplace'::regclass FOR KEY SHARE;
}
teardown	{ ROLLBACK; }


permutation
	b1
	grant1
	read2
	addk2(c1)	# inplace waits
	c1
	read2

# inplace thru KEY SHARE
permutation
	keyshr5
	addk2

# inplace wait NO KEY UPDATE w/ KEY SHARE
permutation
	keyshr5
	b3
	sfnku3
	addk2(r3)
	r3

# reproduce bug in DoesMultiXactIdConflict() call
permutation
	b3
	sfnku3
	keyshr5
	addk2(r3)
	r3

# same-xact rowmark
permutation
	b2
	sfnku2
	addk2
	c2

# same-xact rowmark in multixact
permutation
	keyshr5
	b2
	sfnku2
	addk2
	c2

permutation
	b3
	sfu3
	b1
	grant1(r3)	# acquire LockTuple(), await sfu3 xmax
	read2
	addk2(c1)	# block in LockTuple() behind grant1
	r3			# unblock grant1; addk2 now awaits grant1 xmax
	c1
	read2

permutation
	b2
	sfnku2
	b1
	grant1(addk2)	# acquire LockTuple(), await sfnku2 xmax
	addk2(*)		# block in LockTuple() behind grant1 = deadlock
	c2
	c1
	read2

# SearchSysCacheLocked1() calling LockRelease()
permutation
	b1
	grant1
	b3
	sfu3(c1)	# acquire LockTuple(), await grant1 xmax
	revoke4(r3)	# block in LockTuple() behind sfu3
	c1
	r3			# revoke4 unlocks old tuple and finds new

# SearchSysCacheLocked1() finding a tuple, then no tuple
permutation
	b1
	drop1
	b3
	sfu3(c1)		# acquire LockTuple(), await drop1 xmax
	revoke4(sfu3)	# block in LockTuple() behind sfu3
	c1				# sfu3 locks none; revoke4 unlocks old and finds none
	r3
