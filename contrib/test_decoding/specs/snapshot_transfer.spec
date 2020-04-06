# Test snapshot transfer from subxact to top-level and receival of later snaps.

setup
{
    SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); -- must be first write in xact
    DROP TABLE IF EXISTS dummy;
    CREATE TABLE dummy(i int);
    DROP TABLE IF EXISTS harvest;
    CREATE TABLE harvest(apples int, pears int);
}

teardown
{
    DROP TABLE IF EXISTS harvest;
    DROP TABLE IF EXISTS dummy;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s0"
setup { SET synchronous_commit=on; }
step "s0_begin" { BEGIN; }
step "s0_begin_sub0" { SAVEPOINT s0; }
step "s0_log_assignment" { SELECT pg_current_xact_id() IS NULL; }
step "s0_begin_sub1" { SAVEPOINT s1; }
step "s0_sub_get_base_snap" { INSERT INTO dummy VALUES (0); }
step "s0_insert" { INSERT INTO harvest VALUES (1, 2, 3); }
step "s0_end_sub0" { RELEASE SAVEPOINT s0; }
step "s0_end_sub1" { RELEASE SAVEPOINT s1; }
step "s0_commit" { COMMIT; }
step "s0_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1'); }

session "s1"
setup { SET synchronous_commit=on; }
step "s1_produce_new_snap" { ALTER TABLE harvest ADD COLUMN mangos int; }

# start top-level without base snap, get base snap in subxact, then create new
# snap and make sure it is queued.
permutation "s0_begin" "s0_begin_sub0" "s0_log_assignment" "s0_sub_get_base_snap" "s1_produce_new_snap" "s0_insert" "s0_end_sub0" "s0_commit" "s0_get_changes"

# In previous test, we firstly associated subxact with xact and only then got
# base snap; now nest one more subxact to get snap first and only then (at
# commit) associate it with toplevel.
permutation "s0_begin" "s0_begin_sub0" "s0_log_assignment" "s0_begin_sub1" "s0_sub_get_base_snap" "s1_produce_new_snap" "s0_insert" "s0_end_sub1" "s0_end_sub0" "s0_commit" "s0_get_changes"
