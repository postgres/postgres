SET synchronous_commit = on;

CREATE TABLE test_time(data text);

-- remember the current time
SELECT set_config('test.time_before', NOW()::text, false) IS NOT NULL;

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'test_decoding');

-- a single transaction, to get the commit time
INSERT INTO test_time(data) VALUES ('');

-- parse the commit time from the changeset
SELECT set_config('test.time_after', regexp_replace(data, '^COMMIT \(at (.*)\)$', '\1'), false) IS NOT NULL
FROM pg_logical_slot_peek_changes('regression_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'include-timestamp', '1')
WHERE data ~ 'COMMIT' LIMIT 1;

-- ensure commit time is sane in relation to the previous time
SELECT (time_after - time_before) <= '10 minutes'::interval, time_after >= time_before
FROM (SELECT current_setting('test.time_after')::timestamptz AS time_after, (SELECT current_setting('test.time_before')::timestamptz) AS time_before) AS d;

SELECT pg_drop_replication_slot('regression_slot');
