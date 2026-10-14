# Test for BUG #19555.
#
# Test decoding of a transaction in which a catalog-modifying subtransaction
# was rolled back, where the transaction spans a logical decoding restart
# point.
#
# The tuplecid records written by the aborted subtransaction are queued on the
# toplevel transaction and must be removed when the subtransaction's abort is
# decoded; otherwise they collide with the records of the line pointer that
# gets reused by the toplevel transaction's later catalog insert.
#
#   - s0_insert runs in a subtransaction and produces its first WAL record.
#   - s0_catinsert1 generates an xl_heap_new_cid record in that subtransaction.
#   - s0_rollback aborts the subtransaction.
#   - s2_vacuum reclaims the aborted row's line pointer (dead inserts are
#     removed regardless of the xid horizon, so this is deterministic even
#     while the toplevel transaction is still open); s0_catinsert2 then reuses
#     the same tid with a different cmin.
#   - The transaction stays open across both checkpoints, so the final
#     s1_get_changes replays the transaction's records from the first
#     checkpoint and decodes its commit for output, building the tuplecid
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
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_init" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); }
step "s0_begin" { BEGIN; }
step "s0_savepoint" { SAVEPOINT sp1; }
step "s0_insert" { INSERT INTO tbl1 VALUES (1); }
step "s0_catinsert1" { INSERT INTO user_cat VALUES (5, 'e'); }
step "s0_rollback" { ROLLBACK TO SAVEPOINT sp1; }
step "s0_catinsert2" { INSERT INTO user_cat VALUES (6, 'f'); }
step "s0_commit" { COMMIT; }

session "s1"
setup { SET synchronous_commit=on; }
step "s1_init" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); }
step "s1_checkpoint" { CHECKPOINT; }
# The user_cat rows carry ~1.5kB filler values which would flood the expected
# output; filter them out (decoding still processes them server-side).
step "s1_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0') WHERE data NOT LIKE '%user_cat%'; }

session "s2"
setup { SET synchronous_commit=on; }
step "s2_vacuum" { VACUUM user_cat; }

# The transaction commits only after the second s1_get_changes, so the last
# s1_get_changes both replays the transaction from the first checkpoint and
# decodes its commit for output.
permutation "s0_init" "s0_begin" "s0_savepoint" "s0_insert" "s1_checkpoint" "s1_get_changes" "s0_catinsert1" "s0_rollback" "s2_vacuum" "s0_catinsert2" "s1_checkpoint" "s1_get_changes" "s0_commit" "s1_get_changes"

# Variant with the slot created while the transaction is already open: slot
# creation waits for a consistent point past the open transaction, so the
# transaction is never decoded by this slot at all (no output).  This locks in
# the behavior that decoding cannot start in the middle of a transaction.
permutation "s0_begin" "s0_savepoint" "s0_insert" "s1_init" "s0_catinsert1" "s0_rollback" "s2_vacuum" "s0_catinsert2" "s0_commit" "s1_get_changes"
