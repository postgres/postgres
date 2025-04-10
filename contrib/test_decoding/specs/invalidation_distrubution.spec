# Test that catalog cache invalidation messages are distributed to ongoing
# transactions, ensuring they can access the updated catalog content after
# processing these messages.
setup
{
    SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'pgoutput');
    CREATE TABLE tbl1(val1 integer, val2 integer);
    CREATE PUBLICATION pub;
}

teardown
{
    DROP TABLE tbl1;
    DROP PUBLICATION pub;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s1"
setup { SET synchronous_commit=on; }

step "s1_begin" { BEGIN; }
step "s1_insert_tbl1" { INSERT INTO tbl1 (val1, val2) VALUES (1, 1); }
step "s1_commit" { COMMIT; }

session "s2"
setup { SET synchronous_commit=on; }

step "s2_alter_pub_add_tbl" { ALTER PUBLICATION pub ADD TABLE tbl1; }
step "s2_get_binary_changes" { SELECT count(data) FROM pg_logical_slot_get_binary_changes('isolation_slot', NULL, NULL, 'proto_version', '4', 'publication_names', 'pub') WHERE get_byte(data, 0) = 73; }

# Expect to get one insert change. LOGICAL_REP_MSG_INSERT = 'I'
permutation "s1_insert_tbl1" "s1_begin" "s1_insert_tbl1" "s2_alter_pub_add_tbl" "s1_commit" "s1_insert_tbl1" "s2_get_binary_changes"
