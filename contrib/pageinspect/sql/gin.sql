CREATE TABLE test1 (x int, y int[]);
INSERT INTO test1 VALUES (1, ARRAY[11, 111]);
CREATE INDEX test1_y_idx ON test1 USING gin (y);

\x

SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 0));
SELECT * FROM gin_metapage_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_page_opaque_info(get_raw_page('test1_y_idx', 1));

SELECT * FROM gin_leafpage_items(get_raw_page('test1_y_idx', 1));

DROP TABLE test1;
