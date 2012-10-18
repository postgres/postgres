# DROP INDEX CONCURRENTLY
#
# This test shows that the concurrent write behaviour works correctly
# with the expected output being 2 rows.
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

session "s1"
step "noseq" { SET enable_seqscan = false; }
step "prepi" { PREPARE getrow_idx AS SELECT * FROM test_dc WHERE data=34 ORDER BY id,data; }
step "preps" { PREPARE getrow_seq AS SELECT * FROM test_dc WHERE data::text=34::text ORDER BY id,data; }
step "begin" { BEGIN; }
step "explaini" { EXPLAIN (COSTS OFF) EXECUTE getrow_idx; }
step "explains" { EXPLAIN (COSTS OFF) EXECUTE getrow_seq; }
step "selecti" { EXECUTE getrow_idx; }
step "selects" { EXECUTE getrow_seq; }
step "end" { COMMIT; }

session "s2"
setup { BEGIN; }
step "select2" { SELECT * FROM test_dc WHERE data=34 ORDER BY id,data; }
step "insert2" { INSERT INTO test_dc(data) SELECT * FROM generate_series(1, 100); }
step "end2" { COMMIT; }

session "s3"
step "drop" { DROP INDEX CONCURRENTLY test_dc_data; }

permutation "noseq" "prepi" "preps" "begin" "explaini" "explains" "select2" "drop" "insert2" "end2" "selecti" "selects" "end"
