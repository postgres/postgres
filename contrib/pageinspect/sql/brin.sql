CREATE TABLE test1 (a int, b text);
INSERT INTO test1 VALUES (1, 'one');
CREATE INDEX test1_a_idx ON test1 USING brin (a);

SELECT brin_page_type(get_raw_page('test1_a_idx', 0));
SELECT brin_page_type(get_raw_page('test1_a_idx', 1));
SELECT brin_page_type(get_raw_page('test1_a_idx', 2));

SELECT * FROM brin_metapage_info(get_raw_page('test1_a_idx', 0));
SELECT * FROM brin_metapage_info(get_raw_page('test1_a_idx', 1));

SELECT * FROM brin_revmap_data(get_raw_page('test1_a_idx', 0)) LIMIT 5;
SELECT * FROM brin_revmap_data(get_raw_page('test1_a_idx', 1)) LIMIT 5;

SELECT * FROM brin_page_items(get_raw_page('test1_a_idx', 2), 'test1_a_idx')
    ORDER BY blknum, attnum LIMIT 5;

-- Mask DETAIL messages as these are not portable across architectures.
\set VERBOSITY terse

-- Failures for non-BRIN index.
CREATE INDEX test1_a_btree ON test1 (a);
SELECT brin_page_items(get_raw_page('test1_a_btree', 0), 'test1_a_btree');
SELECT brin_page_items(get_raw_page('test1_a_btree', 0), 'test1_a_idx');

-- Invalid special area size
SELECT brin_page_type(get_raw_page('test1', 0));
SELECT * FROM brin_metapage_info(get_raw_page('test1', 0));
SELECT * FROM brin_revmap_data(get_raw_page('test1', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT brin_page_type(decode(repeat('00', :block_size), 'hex'));
SELECT brin_page_items(decode(repeat('00', :block_size), 'hex'), 'test1_a_idx');
SELECT brin_metapage_info(decode(repeat('00', :block_size), 'hex'));
SELECT brin_revmap_data(decode(repeat('00', :block_size), 'hex'));

-- Test that partial indexes have all pages, including empty ones.
CREATE TABLE test2 (a int);
INSERT INTO test2 SELECT i FROM generate_series(1,1000) s(i);

-- No rows match the index predicate, make sure the index has the right number
-- of ranges (same as number of page ranges).
CREATE INDEX ON test2 USING brin (a) WITH (pages_per_range=1) WHERE (a IS NULL);

ANALYZE test2;

-- Does the index have one summary of the relation?
SELECT (COUNT(*) = (SELECT relpages FROM pg_class WHERE relname = 'test2')) AS ranges_do_match
 FROM generate_series((SELECT (lastrevmappage + 1) FROM brin_metapage_info(get_raw_page('test2_a_idx', 0))),
                      (SELECT (relpages - 1) FROM pg_class WHERE relname = 'test2_a_idx')) AS pages(p),
      LATERAL brin_page_items(get_raw_page('test2_a_idx', p), 'test2_a_idx') AS items;

DROP TABLE test1;
DROP TABLE test2;
