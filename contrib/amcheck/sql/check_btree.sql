CREATE TABLE bttest_a(id int8);
CREATE TABLE bttest_b(id int8);
CREATE TABLE bttest_multi(id int8, data int8);
CREATE TABLE delete_test_table (a bigint, b bigint, c bigint, d bigint);

-- Stabalize tests
ALTER TABLE bttest_a SET (autovacuum_enabled = false);
ALTER TABLE bttest_b SET (autovacuum_enabled = false);
ALTER TABLE bttest_multi SET (autovacuum_enabled = false);
ALTER TABLE delete_test_table SET (autovacuum_enabled = false);

INSERT INTO bttest_a SELECT * FROM generate_series(1, 100000);
INSERT INTO bttest_b SELECT * FROM generate_series(100000, 1, -1);
INSERT INTO bttest_multi SELECT i, i%2  FROM generate_series(1, 100000) as i;

CREATE INDEX bttest_a_idx ON bttest_a USING btree (id);
CREATE INDEX bttest_b_idx ON bttest_b USING btree (id);
CREATE UNIQUE INDEX bttest_multi_idx ON bttest_multi
USING btree (id) INCLUDE (data);

CREATE ROLE bttest_role;

-- verify permissions are checked (error due to function not callable)
SET ROLE bttest_role;
SELECT bt_index_check('bttest_a_idx'::regclass);
SELECT bt_index_parent_check('bttest_a_idx'::regclass);
RESET ROLE;

-- we, intentionally, don't check relation permissions - it's useful
-- to run this cluster-wide with a restricted account, and as tested
-- above explicit permission has to be granted for that.
GRANT EXECUTE ON FUNCTION bt_index_check(regclass) TO bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_parent_check(regclass) TO bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_check(regclass, boolean) TO bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_parent_check(regclass, boolean) TO bttest_role;
SET ROLE bttest_role;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_a_idx');
RESET ROLE;

-- verify plain tables are rejected (error)
SELECT bt_index_check('bttest_a');
SELECT bt_index_parent_check('bttest_a');

-- verify non-existing indexes are rejected (error)
SELECT bt_index_check(17);
SELECT bt_index_parent_check(17);

-- verify wrong index types are rejected (error)
BEGIN;
CREATE INDEX bttest_a_brin_idx ON bttest_a USING brin(id);
SELECT bt_index_parent_check('bttest_a_brin_idx');
ROLLBACK;

-- normal check outside of xact
SELECT bt_index_check('bttest_a_idx');
-- more expansive tests
SELECT bt_index_check('bttest_a_idx', true);
SELECT bt_index_parent_check('bttest_b_idx', true);

BEGIN;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_b_idx');
-- make sure we don't have any leftover locks
SELECT * FROM pg_locks
WHERE relation = ANY(ARRAY['bttest_a', 'bttest_a_idx', 'bttest_b', 'bttest_b_idx']::regclass[])
    AND pid = pg_backend_pid();
COMMIT;

-- normal check outside of xact for index with included columns
SELECT bt_index_check('bttest_multi_idx');
-- more expansive test for index with included columns
SELECT bt_index_parent_check('bttest_multi_idx', true);

-- repeat expansive test for index built using insertions
TRUNCATE bttest_multi;
INSERT INTO bttest_multi SELECT i, i%2  FROM generate_series(1, 100000) as i;
SELECT bt_index_parent_check('bttest_multi_idx', true);

--
-- Test for multilevel page deletion/downlink present checks
--
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,80000) i;
ALTER TABLE delete_test_table ADD PRIMARY KEY (a,b,c,d);
DELETE FROM delete_test_table WHERE a > 40000;
VACUUM delete_test_table;
DELETE FROM delete_test_table WHERE a > 10;
VACUUM delete_test_table;
SELECT bt_index_parent_check('delete_test_table_pkey', true);

-- cleanup
DROP TABLE bttest_a;
DROP TABLE bttest_b;
DROP TABLE bttest_multi;
DROP TABLE delete_test_table;
DROP OWNED BY bttest_role; -- permissions
DROP ROLE bttest_role;
