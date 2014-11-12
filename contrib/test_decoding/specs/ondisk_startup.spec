# Force usage of ondisk decoding snapshots to test that code path.
setup
{
    DROP TABLE IF EXISTS do_write;
    CREATE TABLE do_write(id serial primary key);
}

teardown
{
    DROP TABLE do_write;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}


session "s1"
setup { SET synchronous_commit=on; }

step "s1init" {SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding');}
step "s1start" {SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', 'false');}
step "s1insert" { INSERT INTO do_write DEFAULT VALUES; }
step "s1checkpoint" { CHECKPOINT; }
step "s1alter" { ALTER TABLE do_write ADD COLUMN addedbys1 int; }

session "s2"
setup { SET synchronous_commit=on; }

step "s2txid" { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT txid_current() IS NULL; }
step "s2alter" { ALTER TABLE do_write ADD COLUMN addedbys2 int; }
step "s2c" { COMMIT; }


session "s3"
setup { SET synchronous_commit=on; }

step "s3txid" { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT txid_current() IS NULL; }
step "s3c" { COMMIT; }

# Force usage of ondisk snapshot by starting and not finishing a
# transaction with a assigned xid after consistency has been
# reached. In combination with a checkpoint forcing a snapshot to be
# written and a new restart point computed that'll lead to the usage
# of the snapshot.
permutation "s2txid" "s1init" "s3txid" "s2alter" "s2c" "s1insert" "s1checkpoint" "s1start" "s1insert" "s1alter" "s1insert" "s1start"
