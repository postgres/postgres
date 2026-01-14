# Test parallel replication origin manipulations; ensure local_lsn can be
# updated by all attached sessions.

setup
{
    SELECT pg_replication_origin_create('origin');
    CREATE UNLOGGED TABLE local_lsn_store (session int, lsn pg_lsn);
}

teardown
{
    SELECT pg_replication_origin_drop('origin');
    DROP TABLE local_lsn_store;
}

session "s0"
setup { SET synchronous_commit = on; }
step "s0_setup" { SELECT pg_replication_origin_session_setup('origin'); }
step "s0_is_setup" { SELECT pg_replication_origin_session_is_setup(); }
step "s0_add_message" {
    SELECT 1
    FROM pg_logical_emit_message(true, 'prefix', 'message on s0');
}
step "s0_store_lsn" {
    INSERT INTO local_lsn_store
    SELECT 0, local_lsn FROM pg_replication_origin_status;
}
step "s0_compare" {
    SELECT s0.lsn < s1.lsn
    FROM local_lsn_store as s0, local_lsn_store as s1
    WHERE s0.session = 0 AND s1.session = 1;
}
step "s0_reset" { SELECT pg_replication_origin_session_reset(); }

session "s1"
setup { SET synchronous_commit = on; }
step "s1_setup" {
    SELECT pg_replication_origin_session_setup('origin', pid)
    FROM pg_stat_activity
    WHERE application_name = 'isolation/parallel_session_origin/s0';
}
step "s1_is_setup" { SELECT pg_replication_origin_session_is_setup(); }
step "s1_add_message" {
    SELECT 1
    FROM pg_logical_emit_message(true, 'prefix', 'message on s1');
}
step "s1_store_lsn" {
    INSERT INTO local_lsn_store
    SELECT 1, local_lsn FROM pg_replication_origin_status;
}
step "s1_reset" { SELECT pg_replication_origin_session_reset(); }

# Firstly s0 attaches to a origin and s1 attaches to the same. Both sessions
# commits a transaction and store the local_lsn of the replication origin.
# Compare LSNs and expect latter transaction (done by s1) has larger local_lsn.
permutation "s0_setup" "s0_is_setup" "s1_setup" "s1_is_setup" "s0_add_message" "s0_store_lsn" "s1_add_message" "s1_store_lsn" "s0_compare" "s1_reset" "s0_reset"

# Test that the origin cannot be released if another session is actively using
# it.
permutation "s0_setup" "s0_is_setup" "s1_setup" "s1_is_setup" "s0_reset" "s1_reset" "s0_reset"
