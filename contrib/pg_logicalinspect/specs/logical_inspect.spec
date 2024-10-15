# Test the pg_logicalinspect functions: that needs some permutation to
# ensure that we are creating multiple logical snapshots and that one of them
# contains ongoing catalogs changes.
setup
{
    DROP TABLE IF EXISTS tbl1;
    CREATE TABLE tbl1 (val1 integer, val2 integer);
    CREATE EXTENSION pg_logicalinspect;
}

teardown
{
    DROP TABLE tbl1;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
    DROP EXTENSION pg_logicalinspect;
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_init" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); }
step "s0_begin" { BEGIN; }
step "s0_savepoint" { SAVEPOINT sp1; }
step "s0_truncate" { TRUNCATE tbl1; }
step "s0_insert" { INSERT INTO tbl1 VALUES (1); }
step "s0_commit" { COMMIT; }

session "s1"
setup { SET synchronous_commit=on; }
step "s1_checkpoint" { CHECKPOINT; }
step "s1_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0'); }
step "s1_get_logical_snapshot_meta" { SELECT COUNT(meta.*) from pg_ls_logicalsnapdir(), pg_get_logical_snapshot_meta(name) as meta;}
step "s1_get_logical_snapshot_info" { SELECT info.state, info.catchange_count, array_length(info.catchange_xip,1) AS catchange_array_length, info.committed_count, array_length(info.committed_xip,1) AS committed_array_length FROM pg_ls_logicalsnapdir(), pg_get_logical_snapshot_info(name) AS info ORDER BY 2; }

permutation "s0_init" "s0_begin" "s0_savepoint" "s0_truncate" "s1_checkpoint" "s1_get_changes" "s0_commit" "s0_begin" "s0_insert" "s1_checkpoint" "s1_get_changes" "s0_commit" "s1_get_changes" "s1_get_logical_snapshot_info" "s1_get_logical_snapshot_meta"
