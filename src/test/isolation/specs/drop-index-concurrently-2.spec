setup
{
	CREATE TABLE test_dc(id serial primary key, data int);
	CREATE INDEX test_dc_data ON test_dc(data);
}

session "s1"
setup { BEGIN; }
step "explain" { EXPLAIN (COSTS OFF) SELECT * FROM test_dc WHERE data=34343; }
step "rollback" { ROLLBACK; }
step "droptab" { DROP TABLE test_dc; }
step "selecti" { SELECT indexrelid::regclass, indisvalid, indisready FROM pg_index WHERE indexrelid = 'test_dc_data'::regclass; }
step "dropi" { DROP INDEX test_dc_data; }

session "s2"
step "drop" { DROP INDEX CONCURRENTLY test_dc_data; }

session "s3"
step "cancel" { SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query = 'DROP INDEX CONCURRENTLY test_dc_data;'; }

permutation "explain" "drop" "cancel" "rollback" "droptab" "selecti" "dropi"
