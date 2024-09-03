# Test that a slot creation skips to restore serialized snapshot to reach
# the consistent state.

setup
{
    DROP TABLE IF EXISTS tbl;
    CREATE TABLE tbl (val1 integer);
}

teardown
{
    DROP TABLE tbl;
    SELECT 'stop' FROM pg_drop_replication_slot('slot0');
    SELECT 'stop' FROM pg_drop_replication_slot('slot1');
}

session "s0"
setup { SET synchronous_commit = on; }
step "s0_init" { SELECT 'init' FROM pg_create_logical_replication_slot('slot0', 'test_decoding'); }
step "s0_begin" { BEGIN; }
step "s0_insert1" { INSERT INTO tbl VALUES (1); }
step "s0_insert2" { INSERT INTO tbl VALUES (2); }
step "s0_commit" { COMMIT; }

session "s1"
setup { SET synchronous_commit = on; }
step "s1_init" { SELECT 'init' FROM pg_create_logical_replication_slot('slot1', 'test_decoding'); }
step "s1_get_changes_slot0" { SELECT data FROM pg_logical_slot_get_changes('slot0', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0'); }
step "s1_get_changes_slot1" { SELECT data FROM pg_logical_slot_get_changes('slot1', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0'); }

session "s2"
setup { SET synchronous_commit = on ;}
step "s2_checkpoint" { CHECKPOINT; }
step "s2_get_changes_slot0" { SELECT data FROM pg_logical_slot_get_changes('slot0', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0'); }


# While 'slot1' creation by "s1_init" waits for s0-transaction to commit, the
# RUNNING_XACTS record is written by "s2_checkpoint" and "s2_get_changes_slot1"
# serializes consistent snapshots to the disk at LSNs where are before
# s0-transaction's commit. After s0-transaction commits, "s1_init" resumes but
# must not restore any serialized snapshots and will reach the consistent state
# when decoding a RUNNING_XACTS record generated after s0-transaction's commit.
# We check if the get_changes on 'slot1' will not return any s0-transaction's
# changes as its confirmed_flush_lsn will be after the s0-transaction's commit
# record.
permutation "s0_init" "s0_begin" "s0_insert1" "s1_init" "s2_checkpoint" "s2_get_changes_slot0" "s0_insert2" "s0_commit" "s1_get_changes_slot0" "s1_get_changes_slot1"
