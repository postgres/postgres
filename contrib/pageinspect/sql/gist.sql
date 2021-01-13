CREATE TABLE test_gist AS SELECT point(i,i) p, i::text t FROM
    generate_series(1,1000) i;
CREATE INDEX test_gist_idx ON test_gist USING gist (p);

-- Page 0 is the root, the rest are leaf pages
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist_idx', 0));
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist_idx', 1));
SELECT * FROM gist_page_opaque_info(get_raw_page('test_gist_idx', 2));

SELECT * FROM gist_page_items(get_raw_page('test_gist_idx', 0), 'test_gist_idx');
SELECT * FROM gist_page_items(get_raw_page('test_gist_idx', 1), 'test_gist_idx') LIMIT 5;

-- gist_page_items_bytea prints the raw key data as a bytea. The output of that is
-- platform-dependent (endianess), so omit the actual key data from the output.
SELECT itemoffset, ctid, itemlen FROM gist_page_items_bytea(get_raw_page('test_gist_idx', 0));

DROP TABLE test_gist;
