CREATE TABLE test1 (a int8, b text);
INSERT INTO test1 VALUES (72057594037927937, 'text');
CREATE INDEX test1_a_idx ON test1 USING btree (a);

\x

SELECT * FROM bt_metap('test1_a_idx');

SELECT * FROM bt_page_stats('test1_a_idx', -1);
SELECT * FROM bt_page_stats('test1_a_idx', 0);
SELECT * FROM bt_page_stats('test1_a_idx', 1);
SELECT * FROM bt_page_stats('test1_a_idx', 2);

SELECT * FROM bt_page_items('test1_a_idx', -1);
SELECT * FROM bt_page_items('test1_a_idx', 0);
SELECT * FROM bt_page_items('test1_a_idx', 1);
SELECT * FROM bt_page_items('test1_a_idx', 2);

SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', -1));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 0));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 1));
SELECT * FROM bt_page_items(get_raw_page('test1_a_idx', 2));

-- Failure when using a non-btree index.
CREATE INDEX test1_a_hash ON test1 USING hash(a);
SELECT bt_metap('test1_a_hash');
SELECT bt_page_stats('test1_a_hash', 0);
SELECT bt_page_items('test1_a_hash', 0);

-- Failure with incorrect page size
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes.
\set VERBOSITY terse
SELECT bt_page_items('aaa'::bytea);
\set VERBOSITY default

DROP TABLE test1;
