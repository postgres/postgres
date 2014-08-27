# Test NOWAIT on an updated tuple chain

setup
{

  DROP TABLE IF EXISTS test_nowait;
  CREATE TABLE test_nowait (
	id integer PRIMARY KEY,
	value integer not null
  );

  INSERT INTO test_nowait
  SELECT x,x FROM generate_series(1,2) x;
}

teardown
{
  DROP TABLE test_nowait;
}

session "sl1"
step "sl1_prep" {
	PREPARE sl1_run AS SELECT id FROM test_nowait WHERE pg_advisory_lock(0) is not null FOR UPDATE NOWAIT;
}
step "sl1_exec" {
	BEGIN ISOLATION LEVEL READ COMMITTED;
	EXECUTE sl1_run;
	SELECT xmin, xmax, ctid, * FROM test_nowait;
}
teardown { COMMIT; }

# A session that's used for an UPDATE of the rows to be locked, for when we're testing ctid
# chain following.
session "upd"
step "upd_getlock" {
	SELECT pg_advisory_lock(0);
}
step "upd_doupdate" {
	BEGIN ISOLATION LEVEL READ COMMITTED;
	UPDATE test_nowait SET value = value WHERE id % 2 = 0;
	COMMIT;
}
step "upd_releaselock" {
	SELECT pg_advisory_unlock(0);
}

# A session that acquires locks that sl1 is supposed to avoid blocking on
session "lk1"
step "lk1_doforshare" {
	BEGIN ISOLATION LEVEL READ COMMITTED;
	SELECT id FROM test_nowait WHERE id % 2 = 0 FOR SHARE;
}
teardown {
	COMMIT;
}

permutation "sl1_prep" "upd_getlock" "sl1_exec" "upd_doupdate" "lk1_doforshare" "upd_releaselock"
