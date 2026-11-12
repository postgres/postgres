setup
{
    DROP TABLE IF EXISTS tbl1;
    DROP TABLE IF EXISTS tbl2;
    DROP TABLE IF EXISTS tbl_part, tbl_part_p1;
    CREATE TABLE tbl1(val1 integer, val2 integer);
    CREATE TABLE tbl2(val1 integer, val2 integer);
    CREATE TABLE tbl_part(val1 integer) PARTITION BY LIST (val1);
    CREATE TABLE tbl_part_p1 PARTITION OF tbl_part FOR VALUES IN (1);
}

teardown
{
    DROP PUBLICATION IF EXISTS pub;
    DROP TABLE IF EXISTS tbl_part, tbl_part_p1;
    DROP TABLE tbl1;
    DROP TABLE tbl2;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s1"
setup { SET synchronous_commit=on; }

step "s1_init" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); }
step "s1_init_pgoutput" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'pgoutput'); }
step "s1_begin" { BEGIN; }
step "s1_insert_tbl1" { INSERT INTO tbl1 (val1, val2) VALUES (1, 1); }
step "s1_insert_tbl2" { INSERT INTO tbl2 (val1, val2) VALUES (1, 1); }
step "s1_insert_tbl2_3col" { INSERT INTO tbl2 (val1, val2, val3) VALUES (1, 1, 1); }
step "s1_select_part" { SELECT * FROM tbl_part; }
step "s1_cancel_s2" { SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE application_name = 'isolation/concurrent_ddl_dml/s2'; }
step "s1_create_pub" { CREATE PUBLICATION pub FOR ALL TABLES; }
step "s1_insert_part" { INSERT INTO tbl_part_p1 VALUES (1); }
step "s1_commit" { COMMIT; }

session "s2"
setup { SET synchronous_commit=on; }

step "s2_alter_tbl1_float" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE float; }
step "s2_alter_tbl1_char" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE character varying; }
step "s2_alter_tbl1_boolean" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE boolean; }

step "s2_alter_tbl2_float" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE float; }
step "s2_alter_tbl2_char" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE character varying; }
step "s2_alter_tbl2_text" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE text; }
step "s2_alter_tbl2_boolean" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE boolean; }

step "s2_alter_tbl2_add_int" { ALTER TABLE tbl2 ADD COLUMN val3 INTEGER; }
step "s2_alter_tbl2_add_float" { ALTER TABLE tbl2 ADD COLUMN val3 FLOAT; }
step "s2_alter_tbl2_add_char" { ALTER TABLE tbl2 ADD COLUMN val3 character varying; }
step "s2_alter_tbl2_add_text" { ALTER TABLE tbl2 ADD COLUMN val3 TEXT; }
step "s2_alter_tbl2_drop_3rd_col" { ALTER TABLE tbl2 DROP COLUMN val3; }
step "s2_alter_tbl2_3rd_char" { ALTER TABLE tbl2 ALTER COLUMN val3 TYPE character varying; }
step "s2_alter_tbl2_3rd_text" { ALTER TABLE tbl2 ALTER COLUMN val3 TYPE text; }
step "s2_alter_tbl2_3rd_int" { ALTER TABLE tbl2 ALTER COLUMN val3 TYPE int USING val3::integer; }
step "s2_detach_part" { ALTER TABLE tbl_part DETACH PARTITION tbl_part_p1 CONCURRENTLY; }

step "s2_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1'); }
step "s2_get_binary_changes" { SELECT count(*) > 0 FROM pg_logical_slot_get_binary_changes('isolation_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub'); }



permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_float" "s1_insert_tbl2" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl1_float" "s1_insert_tbl2" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_char" "s1_insert_tbl2" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl1_char" "s1_insert_tbl2" "s1_commit" "s2_get_changes"

permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2" "s2_alter_tbl1_float" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2" "s2_alter_tbl1_char" "s1_commit" "s2_get_changes"

permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_float" "s1_insert_tbl2" "s2_alter_tbl1_float" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_char" "s1_insert_tbl2" "s2_alter_tbl1_char" "s1_commit" "s2_get_changes"

permutation "s1_init" "s2_alter_tbl2_char" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_text" "s1_insert_tbl2" "s1_commit" "s2_get_changes"
permutation "s1_init" "s2_alter_tbl2_char" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_text" "s1_insert_tbl2" "s2_alter_tbl1_char" "s1_commit" "s2_get_changes"

permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_boolean" "s1_insert_tbl2" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_boolean" "s1_insert_tbl2" "s2_alter_tbl1_boolean" "s1_commit" "s2_get_changes"

permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_add_int" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2" "s1_commit" "s1_begin" "s2_alter_tbl2_add_int" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes"

permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_add_float" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2" "s1_commit" "s1_begin" "s2_alter_tbl2_add_float" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes"

permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_add_char" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes"
permutation "s1_init" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2" "s1_commit" "s1_begin" "s2_alter_tbl2_add_char" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes"

permutation "s1_init" "s2_alter_tbl2_add_int" "s1_begin" "s1_insert_tbl2_3col" "s2_alter_tbl2_drop_3rd_col" "s1_commit" "s2_get_changes"
permutation "s1_init" "s2_alter_tbl2_add_int" "s1_begin" "s1_insert_tbl2_3col" "s2_alter_tbl2_drop_3rd_col" "s1_insert_tbl2" "s1_commit" "s1_insert_tbl2" "s2_get_changes"

permutation "s1_init" "s2_alter_tbl2_add_int" "s1_begin" "s1_insert_tbl2_3col" "s2_alter_tbl2_drop_3rd_col" "s1_commit" "s2_get_changes" "s2_alter_tbl2_add_text" "s1_begin" "s1_insert_tbl2_3col" "s2_alter_tbl2_3rd_char" "s1_insert_tbl2_3col" "s1_commit" "s2_get_changes" "s2_alter_tbl2_3rd_int" "s1_insert_tbl2_3col" "s2_get_changes"

permutation "s1_init" "s2_alter_tbl2_add_char" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2_3col" "s2_alter_tbl2_3rd_text" "s1_insert_tbl2_3col" "s1_commit" "s1_insert_tbl2_3col" "s2_get_changes"
permutation "s1_init" "s2_alter_tbl2_add_text" "s1_begin" "s1_insert_tbl1" "s1_insert_tbl2_3col" "s2_alter_tbl2_3rd_char" "s1_insert_tbl2_3col" "s1_commit" "s1_insert_tbl2_3col" "s2_get_changes"

permutation "s1_init" "s2_alter_tbl2_add_char" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_3rd_text" "s1_insert_tbl2_3col" "s1_commit" "s2_alter_tbl2_drop_3rd_col" "s1_insert_tbl2" "s2_get_changes"
permutation "s1_init" "s2_alter_tbl2_add_text" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_3rd_char" "s1_insert_tbl2_3col" "s1_commit" "s2_alter_tbl2_drop_3rd_col" "s1_insert_tbl2" "s2_get_changes"

permutation "s1_init" "s2_alter_tbl2_add_char" "s1_begin" "s1_insert_tbl1" "s2_alter_tbl2_drop_3rd_col" "s1_insert_tbl1" "s1_commit" "s2_get_changes"

# Verify that pgoutput can decode changes to a partition whose concurrent
# detach was canceled, when the relation is still marked as a partition in
# pg_class but already reports no ancestors.
permutation "s1_init_pgoutput" "s1_begin" "s1_select_part" "s2_detach_part" "s1_cancel_s2"("s2_detach_part") "s1_commit" "s1_create_pub" "s1_insert_part" "s2_get_binary_changes"
