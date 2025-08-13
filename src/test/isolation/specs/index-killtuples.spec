# Basic testing of killtuples / kill_prior_tuples / all_dead testing
# for various index AMs
#
# This tests just enough to ensure that the kill* routines are actually
# executed and does something approximately reasonable. It's *not* sufficient
# testing for adding killitems support to a new AM!
#
# This doesn't really need to be an isolation test, it could be written as a
# regular regression test. However, writing it as an isolation test ends up a
# *lot* less verbose.

setup
{
    CREATE TABLE counter(heap_accesses int);
    INSERT INTO counter(heap_accesses) VALUES (0);
}

teardown
{
    DROP TABLE counter;
}

session s1
# to ensure GUCs are reset
setup { RESET ALL; }

step disable_seq { SET enable_seqscan = false; }

step disable_bitmap { SET enable_bitmapscan = false; }

# use a temporary table to make sure no other session can interfere with
# visibility determinations
step create_table { CREATE TEMPORARY TABLE kill_prior_tuple(key int not null, cat text not null); }

step fill_10 { INSERT INTO kill_prior_tuple(key, cat) SELECT g.i, 'a' FROM generate_series(1, 10) g(i); }

step fill_500 { INSERT INTO kill_prior_tuple(key, cat) SELECT g.i, 'a' FROM generate_series(1, 500) g(i); }

# column-less select to make output easier to read
step flush { SELECT FROM pg_stat_force_next_flush(); }

step measure { UPDATE counter SET heap_accesses = (SELECT heap_blks_read + heap_blks_hit FROM pg_statio_all_tables WHERE relname = 'kill_prior_tuple'); }

step result { SELECT heap_blks_read + heap_blks_hit - counter.heap_accesses AS new_heap_accesses FROM counter, pg_statio_all_tables WHERE relname = 'kill_prior_tuple'; }

step access { EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM kill_prior_tuple WHERE key = 1; }

step delete { DELETE FROM kill_prior_tuple; }

step drop_table { DROP TABLE IF EXISTS kill_prior_tuple; }

### steps for testing btree indexes ###
step create_btree { CREATE INDEX kill_prior_tuple_btree ON kill_prior_tuple USING btree (key); }

### steps for testing gist indexes ###
# Creating the extensions takes time, so we don't want to do so when testing
# other AMs
step create_ext_btree_gist { CREATE EXTENSION btree_gist; }
step drop_ext_btree_gist { DROP EXTENSION btree_gist; }
step create_gist { CREATE INDEX kill_prior_tuple_gist ON kill_prior_tuple USING gist (key); }

### steps for testing gin indexes ###
# See create_ext_btree_gist
step create_ext_btree_gin { CREATE EXTENSION btree_gin; }
step drop_ext_btree_gin { DROP EXTENSION btree_gin; }
step create_gin { CREATE INDEX kill_prior_tuple_gin ON kill_prior_tuple USING gin (key); }

### steps for testing hash indexes ###
step create_hash { CREATE INDEX kill_prior_tuple_hash ON kill_prior_tuple USING hash (key); }


# test killtuples with btree index
permutation
  create_table fill_500 create_btree flush
  disable_seq disable_bitmap
  # show each access to non-deleted tuple increments heap_blks_*
  measure access flush result
  measure access flush result
  delete flush
  # first access after accessing deleted tuple still needs to access heap
  measure access flush result
  # but after kill_prior_tuple did its thing, we shouldn't access heap anymore
  measure access flush result
  drop_table

# Same as first permutation, except testing gist
permutation
  create_table fill_500 create_ext_btree_gist create_gist flush
  disable_seq disable_bitmap
  measure access flush result
  measure access flush result
  delete flush
  measure access flush result
  measure access flush result
  drop_table drop_ext_btree_gist

# Test gist, but with fewer rows - shows that killitems doesn't work anymore!
permutation
  create_table fill_10 create_ext_btree_gist create_gist flush
  disable_seq disable_bitmap
  measure access flush result
  measure access flush result
  delete flush
  measure access flush result
  measure access flush result
  drop_table drop_ext_btree_gist

# Same as first permutation, except testing hash
permutation
  create_table fill_500 create_hash flush
  disable_seq disable_bitmap
  measure access flush result
  measure access flush result
  delete flush
  measure access flush result
  measure access flush result
  drop_table

# # Similar to first permutation, except that gin does not have killtuples support
permutation
  create_table fill_500 create_ext_btree_gin create_gin flush
  disable_seq
  delete flush
  measure access flush result
  # will still fetch from heap
  measure access flush result
  drop_table drop_ext_btree_gin
