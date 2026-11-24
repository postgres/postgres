# Test that an index built over broken HOT chains is not used by a snapshot
# that can still see the old row versions.
#
# CREATE INDEX indexes only the live member of a HOT chain, under the TID of
# the chain's root.  When the heap scan of the build skips an older member
# that some snapshot can still see, the build must set pg_index.indcheckxmin,
# which keeps such a snapshot from using the index (see README.HOT).  s2
# requests one parallel worker per build.  In a parallel build, each
# participant must tell the leader whether it skipped such a tuple.

setup
{
    -- No index and a low fillfactor: every UPDATE is a HOT update
    CREATE TABLE broken_hot (a int4, b int4[])
      WITH (autovacuum_enabled = off, fillfactor = 50, parallel_workers = 1);
    INSERT INTO broken_hot SELECT 1, ARRAY[1] FROM generate_series(1, 1000);
}

teardown
{
    DROP TABLE broken_hot;
}


session s1

# s1 uses an index whenever it is allowed to
setup
{
    SET enable_seqscan = off;
    BEGIN ISOLATION LEVEL REPEATABLE READ;
}

# s1's snapshot sees 1000 rows with the old values and none with the new
# values, before and after s2 builds its index
step s1_a_old { SELECT count(*) FROM broken_hot WHERE a = 1; }
step s1_a_new { SELECT count(*) FROM broken_hot WHERE a = 2; }
step s1_b_old { SELECT count(*) FROM broken_hot WHERE b @> ARRAY[1]; }
step s1_b_new { SELECT count(*) FROM broken_hot WHERE b @> ARRAY[2]; }
step s1_commit { COMMIT; }


session s2

setup
{
    SET max_parallel_maintenance_workers = 1;
}

step s2_update { UPDATE broken_hot SET a = 2, b = ARRAY[2]; }
step s2_btree { CREATE INDEX broken_hot_idx ON broken_hot USING btree (a); }
step s2_brin { CREATE INDEX broken_hot_idx ON broken_hot USING brin (a); }
step s2_gin { CREATE INDEX broken_hot_idx ON broken_hot USING gin (b); }

# The build must set indcheckxmin
step s2_indcheckxmin
{
    SELECT indcheckxmin FROM pg_index
      WHERE indexrelid = 'broken_hot_idx'::regclass;
}

permutation s1_a_old s1_a_new s2_update s2_btree s2_indcheckxmin
  s1_a_old s1_a_new s1_commit
permutation s1_a_old s1_a_new s2_update s2_brin s2_indcheckxmin
  s1_a_old s1_a_new s1_commit
permutation s1_b_old s1_b_new s2_update s2_gin s2_indcheckxmin
  s1_b_old s1_b_new s1_commit
