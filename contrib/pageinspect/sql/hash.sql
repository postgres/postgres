CREATE TABLE test_hash (a int, b text);
INSERT INTO test_hash VALUES (1, 'one');
CREATE INDEX test_hash_a_idx ON test_hash USING hash (a);

\x

SELECT hash_page_type(get_raw_page('test_hash_a_idx', 0));
SELECT hash_page_type(get_raw_page('test_hash_a_idx', 1));
SELECT hash_page_type(get_raw_page('test_hash_a_idx', 2));
SELECT hash_page_type(get_raw_page('test_hash_a_idx', 3));
SELECT hash_page_type(get_raw_page('test_hash_a_idx', 4));
SELECT hash_page_type(get_raw_page('test_hash_a_idx', 5));
SELECT hash_page_type(get_raw_page('test_hash_a_idx', 6));


SELECT * FROM hash_bitmap_info('test_hash_a_idx', -1);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 0);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 1);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 2);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 3);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 4);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 5);
SELECT * FROM hash_bitmap_info('test_hash_a_idx', 6);


SELECT magic, version, ntuples, bsize, bmsize, bmshift, maxbucket, highmask,
lowmask, ovflpoint, firstfree, nmaps, procid, spares, mapp FROM
hash_metapage_info(get_raw_page('test_hash_a_idx', 0));

SELECT magic, version, ntuples, bsize, bmsize, bmshift, maxbucket, highmask,
lowmask, ovflpoint, firstfree, nmaps, procid, spares, mapp FROM
hash_metapage_info(get_raw_page('test_hash_a_idx', 1));

SELECT magic, version, ntuples, bsize, bmsize, bmshift, maxbucket, highmask,
lowmask, ovflpoint, firstfree, nmaps, procid, spares, mapp FROM
hash_metapage_info(get_raw_page('test_hash_a_idx', 2));

SELECT magic, version, ntuples, bsize, bmsize, bmshift, maxbucket, highmask,
lowmask, ovflpoint, firstfree, nmaps, procid, spares, mapp FROM
hash_metapage_info(get_raw_page('test_hash_a_idx', 3));

SELECT magic, version, ntuples, bsize, bmsize, bmshift, maxbucket, highmask,
lowmask, ovflpoint, firstfree, nmaps, procid, spares, mapp FROM
hash_metapage_info(get_raw_page('test_hash_a_idx', 4));

SELECT magic, version, ntuples, bsize, bmsize, bmshift, maxbucket, highmask,
lowmask, ovflpoint, firstfree, nmaps, procid, spares, mapp FROM
hash_metapage_info(get_raw_page('test_hash_a_idx', 5));

SELECT live_items, dead_items, page_size, hasho_prevblkno, hasho_nextblkno,
hasho_bucket, hasho_flag, hasho_page_id FROM
hash_page_stats(get_raw_page('test_hash_a_idx', 0));

SELECT live_items, dead_items, page_size, hasho_prevblkno, hasho_nextblkno,
hasho_bucket, hasho_flag, hasho_page_id FROM
hash_page_stats(get_raw_page('test_hash_a_idx', 1));

SELECT live_items, dead_items, page_size, hasho_prevblkno, hasho_nextblkno,
hasho_bucket, hasho_flag, hasho_page_id FROM
hash_page_stats(get_raw_page('test_hash_a_idx', 2));

SELECT live_items, dead_items, page_size, hasho_prevblkno, hasho_nextblkno,
hasho_bucket, hasho_flag, hasho_page_id FROM
hash_page_stats(get_raw_page('test_hash_a_idx', 3));

SELECT live_items, dead_items, page_size, hasho_prevblkno, hasho_nextblkno,
hasho_bucket, hasho_flag, hasho_page_id FROM
hash_page_stats(get_raw_page('test_hash_a_idx', 4));

SELECT live_items, dead_items, page_size, hasho_prevblkno, hasho_nextblkno,
hasho_bucket, hasho_flag, hasho_page_id FROM
hash_page_stats(get_raw_page('test_hash_a_idx', 5));

SELECT * FROM hash_page_items(get_raw_page('test_hash_a_idx', 0));
SELECT * FROM hash_page_items(get_raw_page('test_hash_a_idx', 1));
SELECT * FROM hash_page_items(get_raw_page('test_hash_a_idx', 2));
SELECT * FROM hash_page_items(get_raw_page('test_hash_a_idx', 3));
SELECT * FROM hash_page_items(get_raw_page('test_hash_a_idx', 4));
SELECT * FROM hash_page_items(get_raw_page('test_hash_a_idx', 5));

-- Failure with non-hash index
CREATE INDEX test_hash_a_btree ON test_hash USING btree (a);
SELECT hash_bitmap_info('test_hash_a_btree', 0);

-- Failure with various modes.
-- Suppress the DETAIL message, to allow the tests to work across various
-- page sizes and architectures.
\set VERBOSITY terse
-- invalid page size
SELECT hash_metapage_info('aaa'::bytea);
SELECT hash_page_items('bbb'::bytea);
SELECT hash_page_stats('ccc'::bytea);
SELECT hash_page_type('ddd'::bytea);
-- invalid special area size
SELECT hash_metapage_info(get_raw_page('test_hash', 0));
SELECT hash_page_items(get_raw_page('test_hash', 0));
SELECT hash_page_stats(get_raw_page('test_hash', 0));
SELECT hash_page_type(get_raw_page('test_hash', 0));
\set VERBOSITY default

-- Tests with all-zero pages.
SHOW block_size \gset
SELECT hash_metapage_info(decode(repeat('00', :block_size), 'hex'));
SELECT hash_page_items(decode(repeat('00', :block_size), 'hex'));
SELECT hash_page_stats(decode(repeat('00', :block_size), 'hex'));
SELECT hash_page_type(decode(repeat('00', :block_size), 'hex'));

DROP TABLE test_hash;
