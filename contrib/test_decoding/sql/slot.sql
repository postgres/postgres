SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_p', 'test_decoding');
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_t', 'test_decoding', true);

SELECT pg_drop_replication_slot('regression_slot_p');
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot_p', 'test_decoding', false);

-- reconnect to clean temp slots
\c

SELECT pg_drop_replication_slot('regression_slot_p');

-- should fail because the temporary slot was dropped automatically
SELECT pg_drop_replication_slot('regression_slot_t');


-- test switching between slots in a session
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot1', 'test_decoding', true);
SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot2', 'test_decoding', true);
SELECT * FROM pg_logical_slot_get_changes('regression_slot1', NULL, NULL);
SELECT * FROM pg_logical_slot_get_changes('regression_slot2', NULL, NULL);
