# GRANT's lock is the catalog tuple xmax.  GRANT doesn't acquire a heavyweight
# lock on the object undergoing an ACL change.  In-place updates, namely
# datfrozenxid, need special code to cope.

setup
{
	CREATE ROLE regress_temp_grantee;
}

teardown
{
	REVOKE ALL ON DATABASE isolation_regression FROM regress_temp_grantee;
	DROP ROLE regress_temp_grantee;
}

# heap_update(pg_database)
session s1
step b1	{ BEGIN; }
step grant1	{
	GRANT TEMP ON DATABASE isolation_regression TO regress_temp_grantee;
}
step c1	{ COMMIT; }

# inplace update
session s2
step vac2	{ VACUUM (FREEZE); }

# observe datfrozenxid
session s3
setup	{
	CREATE TEMP TABLE frozen_witness (x xid);
}
step snap3	{
	INSERT INTO frozen_witness
	SELECT datfrozenxid FROM pg_database WHERE datname = current_catalog;
}
step cmp3	{
	SELECT 'datfrozenxid retreated'
	FROM pg_database
	WHERE datname = current_catalog
		AND age(datfrozenxid) > (SELECT min(age(x)) FROM frozen_witness);
}


permutation snap3 b1 grant1 vac2(c1) snap3 c1 cmp3
