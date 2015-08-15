setup
{
    DROP TABLE IF EXISTS tbl1;
    DROP TABLE IF EXISTS tbl2;
    CREATE TABLE tbl1(val1 integer, val2 integer);
    CREATE TABLE tbl2(val1 integer, val2 integer);
}

teardown
{
    DROP TABLE tbl1;
    DROP TABLE tbl2;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s1"
step "s1_init" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); }
step "s1_begin" { BEGIN; }
step "s1_insert_tbl1" { INSERT INTO tbl1 (val1, val2) VALUES (1, 1); }
step "s1_insert_tbl1_3col" { INSERT INTO tbl1 (val1, val2, val3) VALUES (1, 1, 1); }
step "s1_insert_tbl2" { INSERT INTO tbl2 (val1, val2) VALUES (1, 1); }
step "s1_insert_tbl2_3col" { INSERT INTO tbl2 (val1, val2, val3) VALUES (1, 1, 1); }
step "s1_commit" { COMMIT; }

session "s2"
step "s2_alter_tbl1_float" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE float; }
step "s2_alter_tbl1_char" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE character varying; }
step "s2_alter_tbl1_text" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE text; }
step "s2_alter_tbl1_boolean" { ALTER TABLE tbl1 ALTER COLUMN val2 TYPE boolean; }

step "s2_alter_tbl1_add_int" { ALTER TABLE tbl1 ADD COLUMN val3 INTEGER; }
step "s2_alter_tbl1_add_float" { ALTER TABLE tbl1 ADD COLUMN val3 FLOAT; }
step "s2_alter_tbl1_add_char" { ALTER TABLE tbl1 ADD COLUMN val3 character varying; }
step "s2_alter_tbl1_add_boolean" { ALTER TABLE tbl1 ADD COLUMN val3 BOOLEAN; }
step "s2_alter_tbl1_add_text" { ALTER TABLE tbl1 ADD COLUMN val3 TEXT; }

step "s2_alter_tbl2_float" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE float; }
step "s2_alter_tbl2_char" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE character varying; }
step "s2_alter_tbl2_text" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE text; }
step "s2_alter_tbl2_boolean" { ALTER TABLE tbl2 ALTER COLUMN val2 TYPE boolean; }

step "s2_alter_tbl2_add_int" { ALTER TABLE tbl2 ADD COLUMN val3 INTEGER; }
step "s2_alter_tbl2_add_float" { ALTER TABLE tbl2 ADD COLUMN val3 FLOAT; }
step "s2_alter_tbl2_add_char" { ALTER TABLE tbl2 ADD COLUMN val3 character varying; }
step "s2_alter_tbl2_add_boolean" { ALTER TABLE tbl2 ADD COLUMN val3 BOOLEAN; }
step "s2_alter_tbl2_add_text" { ALTER TABLE tbl2 ADD COLUMN val3 TEXT; }
step "s2_alter_tbl2_drop_3rd_col" { ALTER TABLE tbl2 DROP COLUMN val3; }
step "s2_alter_tbl2_3rd_char" { ALTER TABLE tbl2 ALTER COLUMN val3 TYPE character varying; }
step "s2_alter_tbl2_3rd_text" { ALTER TABLE tbl2 ALTER COLUMN val3 TYPE text; }
step "s2_alter_tbl2_3rd_int" { ALTER TABLE tbl2 ALTER COLUMN val3 TYPE int USING val3::integer; }

step "s2_get_changes" { SELECT regexp_replace(data, 'temp_\d+', 'temp') AS data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1'); }



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
