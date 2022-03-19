# Test that erroring out during logical slot creation is handled properly

session "s1"
setup { SET synchronous_commit=on; }

step s1_b { BEGIN; }
step s1_xid { SELECT 'xid' FROM txid_current(); }
step s1_c { COMMIT; }
step s1_cancel_s2 {
    SELECT pg_cancel_backend(pid)
    FROM pg_stat_activity
    WHERE application_name = 'isolation/slot_creation_error/s2';
}

step s1_terminate_s2 {
    SELECT pg_terminate_backend(pid)
    FROM pg_stat_activity
    WHERE application_name = 'isolation/slot_creation_error/s2';
}

step s1_view_slot {
    SELECT slot_name, slot_type, active FROM pg_replication_slots WHERE slot_name = 'slot_creation_error'
}

step s1_drop_slot {
    SELECT pg_drop_replication_slot('slot_creation_error');
}

session s2
setup { SET synchronous_commit=on; }
step s2_init {
    SELECT 'init' FROM pg_create_logical_replication_slot('slot_creation_error', 'test_decoding');
}

# The tests first start a transaction with an xid assigned in s1, then create
# a slot in s2. The slot creation waits for s1's transaction to end. Instead
# we cancel / terminate s2.
permutation s1_b s1_xid s2_init s1_view_slot s1_cancel_s2(s2_init) s1_view_slot s1_c
permutation s1_b s1_xid s2_init s1_c s1_view_slot s1_drop_slot # check slot creation still works
permutation s1_b s1_xid s2_init s1_terminate_s2(s2_init) s1_c s1_view_slot
# can't run tests after this, due to s2's connection failure
