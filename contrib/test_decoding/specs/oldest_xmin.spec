# Test advancement of the slot's oldest xmin

setup
{
    SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); -- must be first write in xact
    DROP TYPE IF EXISTS basket;
    CREATE TYPE basket AS (apples integer, pears integer, mangos integer);
    DROP TABLE IF EXISTS harvest;
    CREATE TABLE harvest(fruits basket);
}

teardown
{
    DROP TABLE IF EXISTS harvest;
    DROP TYPE IF EXISTS basket;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_begin" { BEGIN; }
step "s0_getxid" { SELECT pg_current_xact_id() IS NULL; }
step "s0_alter" { ALTER TYPE basket DROP ATTRIBUTE mangos; }
step "s0_commit" { COMMIT; }
step "s0_checkpoint" { CHECKPOINT; }
step "s0_vacuum" { VACUUM pg_attribute; }
step "s0_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1'); }
step "s0_advance_slot" { SELECT slot_name FROM pg_replication_slot_advance('isolation_slot', pg_current_wal_lsn()); }

session "s1"
setup { SET synchronous_commit=on; }
step "s1_begin" { BEGIN; }
step "s1_insert" { INSERT INTO harvest VALUES ((1, 2, 3)); }
step "s1_commit" { COMMIT; }

# Checkpoint with following get_changes forces xmin advancement. We do
# get_changes twice because if one more xl_running_xacts record had slipped
# before our CHECKPOINT, xmin will be advanced only on this record, thus not
# reaching value needed for vacuuming corresponding pg_attribute entry. ALTER of
# composite type is a rare form of DDL which allows T1 to see the tuple which
# will be removed (xmax set) before T1 commits. That is, interlocking doesn't
# forbid modifying catalog after someone read it (and didn't commit yet).
permutation "s0_begin" "s0_getxid" "s1_begin" "s1_insert" "s0_alter" "s0_commit" "s0_checkpoint" "s0_get_changes" "s0_get_changes" "s1_commit" "s0_vacuum" "s0_get_changes"

# Perform the same testing process as described above, but use advance_slot to
# forces xmin advancement during fast forward decoding.
permutation "s0_begin" "s0_getxid" "s1_begin" "s1_insert" "s0_alter" "s0_commit" "s0_checkpoint" "s0_advance_slot" "s0_advance_slot" "s1_commit" "s0_vacuum" "s0_get_changes"
