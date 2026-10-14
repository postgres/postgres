# Test for BUG #19555.
#
# Test decoding of a partially rolled back transaction where the aborted part
# is nested: the aborting outer subtransaction never produced any WAL of its
# own, so decoding cannot know its toplevel transaction, while an inner
# subtransaction it rolls back did produce WAL and its tuplecid records still
# have to be removed
#
# The tuplecid records written by the aborted inner subtransaction are queued
# on the toplevel transaction and must be removed when the outer
# subtransaction's abort is decoded; otherwise they collide with the records
# of the line pointer that gets reused by the toplevel transaction's later
# catalog insert.
#
#   - s0_topwrite makes the toplevel transaction produce its first WAL record
#     before any subtransaction exists.
#   - s0_outer establishes the outer savepoint, which never gets a WAL record
#     of its own.
#   - s0_catinsert1 generates an xl_heap_new_cid record in the inner
#     subtransaction, which is then released.
#   - s0_rollback_outer rolls the outer savepoint back.  Its abort record is
#     written once the backend is already in TRANS_ABORT, so it carries no
#     toplevel xid in its header and the outer subtransaction's association
#     with the toplevel can never be established.  The released inner
#     subtransaction is listed in the same abort record, though, and its
#     tuplecid records still must be removed from the toplevel's list.
#   - s2_vacuum reclaims the aborted row's line pointer (dead inserts are
#     removed regardless of the xid horizon, so this is deterministic even
#     while the toplevel transaction is still open); s0_catinsert2 then reuses
#     the same tid with a different cmin.
#   - s1_get_changes then decodes the toplevel commit and builds the tuplecid
#     hash.
setup
{
    DROP TABLE IF EXISTS tbl1;
    DROP TABLE IF EXISTS user_cat;
    CREATE TABLE tbl1 (val1 integer);
    -- Four rows of ~1.5kB fill the first heap page to within a few hundred
    -- bytes, so that vacuuming away the aborted fifth row leaves exactly one
    -- line pointer for the next insert to reuse.
    CREATE TABLE user_cat (c1 int, filler char(1500)) WITH (user_catalog_table = true);
    INSERT INTO user_cat VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd');
}

teardown
{
    DROP TABLE tbl1;
    DROP TABLE user_cat;
    SELECT 'stop' FROM pg_drop_replication_slot('tuplecid_nested_slot');
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_begin" { BEGIN; }
step "s0_topwrite" { INSERT INTO tbl1 VALUES (0); }
step "s0_outer" { SAVEPOINT outer_sp; }
step "s0_inner" { SAVEPOINT inner_sp; }
step "s0_catinsert1" { INSERT INTO user_cat VALUES (5, 'e'); }
step "s0_release_inner" { RELEASE SAVEPOINT inner_sp; }
step "s0_rollback_outer" { ROLLBACK TO SAVEPOINT outer_sp; }
step "s0_catinsert2" { INSERT INTO user_cat VALUES (6, 'f'); }
step "s0_commit" { COMMIT; }

session "s1"
setup { SET synchronous_commit=on; }
step "s1_init" { SELECT 'init' FROM pg_create_logical_replication_slot('tuplecid_nested_slot', 'test_decoding'); }
# The user_cat rows carry ~1.5kB filler values which would flood the expected
# output; filter them out (decoding still processes them server-side).
step "s1_get_changes" { SELECT data FROM pg_logical_slot_get_changes('tuplecid_nested_slot', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0') WHERE data NOT LIKE '%user_cat%'; }

session "s2"
setup { SET synchronous_commit=on; }
step "s2_vacuum" { VACUUM user_cat; }

# The slot is created before the transaction begins, so the final
# s1_get_changes decodes the whole transaction through its commit.
permutation "s1_init" "s0_begin" "s0_topwrite" "s0_outer" "s0_inner" "s0_catinsert1" "s0_release_inner" "s0_rollback_outer" "s2_vacuum" "s0_catinsert2" "s0_commit" "s1_get_changes"
