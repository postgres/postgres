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
step "s1b" { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "s1w" { INSERT INTO do_write DEFAULT VALUES; }
step "s1c" { COMMIT; }
session "s2"
setup { SET synchronous_commit=on; }
step "s2init" {SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding');}
step "s2start" {SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', 'false');}


permutation "s1b" "s1w" "s2init" "s1c" "s2start" "s1b" "s1w" "s1c" "s2start" "s1b" "s1w" "s2start" "s1c" "s2start"
