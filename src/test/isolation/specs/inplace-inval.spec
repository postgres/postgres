# If a heap_update() caller retrieves its oldtup from a cache, it's possible
# for that cache entry to predate an inplace update, causing loss of that
# inplace update.  This arises because the transaction may abort before
# sending the inplace invalidation message to the shared queue.

setup
{
	CREATE TABLE newly_indexed (c int);
}

teardown
{
	DROP TABLE newly_indexed;
}

session s1
step cir1	{ BEGIN; CREATE INDEX i1 ON newly_indexed (c); ROLLBACK; }
step read1	{
	SELECT relhasindex FROM pg_class WHERE oid = 'newly_indexed'::regclass;
}

session s2
step cic2	{ CREATE INDEX i2 ON newly_indexed (c); }

session s3
step cachefill3	{ TABLE newly_indexed; }
step ddl3		{ ALTER TABLE newly_indexed ADD extra int; }


# XXX shows an extant bug.  Adding step read1 at the end would usually print
# relhasindex=f (not wanted).  This does not reach the unwanted behavior under
# -DCATCACHE_FORCE_RELEASE and friends.
permutation
	cachefill3	# populates the pg_class row in the catcache
	cir1	# sets relhasindex=true; rollback discards cache inval
	cic2	# sees relhasindex=true, skips changing it (so no inval)
	ddl3	# cached row as the oldtup of an update, losing relhasindex

# without cachefill3, no bug
permutation cir1 cic2 ddl3 read1
