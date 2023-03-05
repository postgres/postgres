# DROP INDEX CONCURRENTLY
#
# This test shows that the concurrent write behaviour works correctly
# with the expected output being 2 rows at the READ COMMITTED and READ
# UNCOMMITTED transaction isolation levels, and 1 row at the other
# transaction isolation levels.  We ensure this is the case by checking
# the returned rows in an index scan plan and a seq scan plan.
#
setup
{
	CREATE TABLE test_dc(id serial primary key, data int);
	INSERT INTO test_dc(data) SELECT * FROM generate_series(1, 100);
	CREATE INDEX test_dc_data ON test_dc(data);
}

teardown
{
	DROP TABLE test_dc;
}

session s1
step chkiso { SELECT (setting in ('read committed','read uncommitted')) AS is_read_committed FROM pg_settings WHERE name = 'default_transaction_isolation'; }
step prepi { PREPARE getrow_idxscan AS SELECT * FROM test_dc WHERE data = 34 ORDER BY id,data; }
step preps { PREPARE getrow_seqscan AS SELECT * FROM test_dc WHERE data = 34 ORDER BY id,data; }
step begin { BEGIN; }
step disableseq { SET enable_seqscan = false; }
step explaini { EXPLAIN (COSTS OFF) EXECUTE getrow_idxscan; }
step enableseq { SET enable_seqscan = true; }
step explains { EXPLAIN (COSTS OFF) EXECUTE getrow_seqscan; }
step selecti { EXECUTE getrow_idxscan; }
step selects { EXECUTE getrow_seqscan; }
step end { COMMIT; }

session s2
setup { BEGIN; }
step select2 { SELECT * FROM test_dc WHERE data = 34 ORDER BY id,data; }
step insert2 { INSERT INTO test_dc(data) SELECT * FROM generate_series(1, 100); }
step end2 { COMMIT; }

session s3
step drop { DROP INDEX CONCURRENTLY test_dc_data; }

permutation chkiso prepi preps begin disableseq explaini enableseq explains select2 drop insert2 end2 selecti selects end
