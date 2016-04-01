CREATE EXTENSION bloom;

CREATE TABLE tst (
	i	int4,
	t	text
);

INSERT INTO tst SELECT i%10, substr(md5(i::text), 1, 1) FROM generate_series(1,100000) i;
CREATE INDEX bloomidx ON tst USING bloom (i, t) WITH (col1 = 3);

SET enable_seqscan=on;
SET enable_bitmapscan=off;
SET enable_indexscan=off;

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

SET enable_seqscan=off;
SET enable_bitmapscan=on;
SET enable_indexscan=on;

EXPLAIN (COSTS OFF) SELECT count(*) FROM tst WHERE i = 7;
EXPLAIN (COSTS OFF) SELECT count(*) FROM tst WHERE t = '5';
EXPLAIN (COSTS OFF) SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

DELETE FROM tst;
INSERT INTO tst SELECT i%10, substr(md5(i::text), 1, 1) FROM generate_series(1,100000) i;
VACUUM ANALYZE tst;

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

VACUUM FULL tst;

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;
