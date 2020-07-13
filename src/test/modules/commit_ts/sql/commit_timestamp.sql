--
-- Commit Timestamp
--
SHOW track_commit_timestamp;
CREATE TABLE committs_test(id serial, ts timestamptz default now());

INSERT INTO committs_test DEFAULT VALUES;
INSERT INTO committs_test DEFAULT VALUES;
INSERT INTO committs_test DEFAULT VALUES;

SELECT id,
       pg_xact_commit_timestamp(xmin) >= ts,
       pg_xact_commit_timestamp(xmin) <= now(),
       pg_xact_commit_timestamp(xmin) - ts < '60s' -- 60s should give a lot of reserve
FROM committs_test
ORDER BY id;

DROP TABLE committs_test;

SELECT pg_xact_commit_timestamp('0'::xid);
SELECT pg_xact_commit_timestamp('1'::xid);
SELECT pg_xact_commit_timestamp('2'::xid);

SELECT x.xid::text::bigint > 0 as xid_valid,
       x.timestamp > '-infinity'::timestamptz AS ts_low,
       x.timestamp <= now() AS ts_high,
       roident != 0 AS valid_roident
  FROM pg_last_committed_xact() x;

-- Test non-normal transaction ids.
SELECT * FROM pg_xact_commit_timestamp_origin(NULL); -- ok, NULL
SELECT * FROM pg_xact_commit_timestamp_origin('0'::xid); -- error
SELECT * FROM pg_xact_commit_timestamp_origin('1'::xid); -- ok, NULL
SELECT * FROM pg_xact_commit_timestamp_origin('2'::xid); -- ok, NULL

-- Test transaction without replication origin
SELECT txid_current() as txid_no_origin \gset
SELECT x.timestamp > '-infinity'::timestamptz AS ts_low,
       x.timestamp <= now() AS ts_high,
       roident != 0 AS valid_roident
  FROM pg_last_committed_xact() x;
SELECT x.timestamp > '-infinity'::timestamptz AS ts_low,
       x.timestamp <= now() AS ts_high,
       roident != 0 AS valid_roident
  FROM pg_xact_commit_timestamp_origin(:'txid_no_origin') x;

-- Test transaction with replication origin
SELECT pg_replication_origin_create('regress_commit_ts: get_origin') != 0
  AS valid_roident;
SELECT pg_replication_origin_session_setup('regress_commit_ts: get_origin');
SELECT txid_current() as txid_with_origin \gset
SELECT x.timestamp > '-infinity'::timestamptz AS ts_low,
       x.timestamp <= now() AS ts_high,
       r.roname
  FROM pg_last_committed_xact() x, pg_replication_origin r
  WHERE r.roident = x.roident;
SELECT x.timestamp > '-infinity'::timestamptz AS ts_low,
       x.timestamp <= now() AS ts_high,
       r.roname
  FROM pg_xact_commit_timestamp_origin(:'txid_with_origin') x, pg_replication_origin r
  WHERE r.roident = x.roident;

SELECT pg_replication_origin_session_reset();
SELECT pg_replication_origin_drop('regress_commit_ts: get_origin');
