CREATE TABLE test1 (a int8, b int4range);
INSERT INTO test1 VALUES (72057594037927937, '[0,1)');
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
SELECT bt_page_items(get_raw_page('test1_a_hash', 0));
CREATE INDEX test1_b_gist ON test1 USING gist(b);
-- Special area of GiST is the same as btree, this complains about inconsistent
-- leaf data on the page.
SELECT bt_page_items(get_raw_page('test1_b_gist', 0));

-- Several failure modes.
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes and architectures.
\set VERBOSITY terse
-- invalid page size
SELECT bt_page_items('aaa'::bytea);
-- invalid special area size
CREATE INDEX test1_a_brin ON test1 USING brin(a);
SELECT bt_page_items(get_raw_page('test1', 0));
SELECT bt_page_items(get_raw_page('test1_a_brin', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT bt_page_items(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test1;
