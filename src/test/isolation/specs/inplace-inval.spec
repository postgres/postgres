# An inplace update had been able to abort before sending the inplace
# invalidation message to the shared queue.  If a heap_update() caller then
# retrieved its oldtup from a cache, the heap_update() could revert the
# inplace update.

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


permutation
	cachefill3	# populates the pg_class row in the catcache
	cir1	# sets relhasindex=true; rollback discards cache inval
	cic2	# sees relhasindex=true, skips changing it (so no inval)
	ddl3	# cached row as the oldtup of an update, losing relhasindex
	read1	# observe damage

# without cachefill3, no bug
permutation cir1 cic2 ddl3 read1
