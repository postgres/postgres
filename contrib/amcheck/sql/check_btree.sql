-- minimal test, basically just verifying that amcheck
CREATE TABLE bttest_a(id int8);
CREATE TABLE bttest_b(id int8);

INSERT INTO bttest_a SELECT * FROM generate_series(1, 100000);
INSERT INTO bttest_b SELECT * FROM generate_series(100000, 1, -1);

CREATE INDEX bttest_a_idx ON bttest_a USING btree (id);
CREATE INDEX bttest_b_idx ON bttest_b USING btree (id);

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
-- more expansive test
SELECT bt_index_parent_check('bttest_b_idx');

BEGIN;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_b_idx');
-- make sure we don't have any leftover locks
SELECT * FROM pg_locks
WHERE relation = ANY(ARRAY['bttest_a', 'bttest_a_idx', 'bttest_b', 'bttest_b_idx']::regclass[])
    AND pid = pg_backend_pid();
COMMIT;

-- cleanup
DROP TABLE bttest_a;
DROP TABLE bttest_b;
DROP OWNED BY bttest_role; -- permissions
DROP ROLE bttest_role;
