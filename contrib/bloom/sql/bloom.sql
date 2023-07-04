CREATE EXTENSION bloom;

CREATE TABLE tst (
	i	int4,
	t	text
);

INSERT INTO tst SELECT i%10, substr(encode(sha256(i::text::bytea), 'hex'), 1, 1) FROM generate_series(1,2000) i;
CREATE INDEX bloomidx ON tst USING bloom (i, t) WITH (col1 = 3);
ALTER INDEX bloomidx SET (length=80);

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
INSERT INTO tst SELECT i%10, substr(encode(sha256(i::text::bytea), 'hex'), 1, 1) FROM generate_series(1,2000) i;
VACUUM ANALYZE tst;

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

DELETE FROM tst WHERE i > 1 OR t = '5';
VACUUM tst;
INSERT INTO tst SELECT i%10, substr(encode(sha256(i::text::bytea), 'hex'), 1, 1) FROM generate_series(1,2000) i;

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

VACUUM FULL tst;

SELECT count(*) FROM tst WHERE i = 7;
SELECT count(*) FROM tst WHERE t = '5';
SELECT count(*) FROM tst WHERE i = 7 AND t = '5';

-- Try an unlogged table too

CREATE UNLOGGED TABLE tstu (
	i	int4,
	t	text
);

INSERT INTO tstu SELECT i%10, substr(encode(sha256(i::text::bytea), 'hex'), 1, 1) FROM generate_series(1,2000) i;
CREATE INDEX bloomidxu ON tstu USING bloom (i, t) WITH (col2 = 4);

SET enable_seqscan=off;
SET enable_bitmapscan=on;
SET enable_indexscan=on;

EXPLAIN (COSTS OFF) SELECT count(*) FROM tstu WHERE i = 7;
EXPLAIN (COSTS OFF) SELECT count(*) FROM tstu WHERE t = '5';
EXPLAIN (COSTS OFF) SELECT count(*) FROM tstu WHERE i = 7 AND t = '5';

SELECT count(*) FROM tstu WHERE i = 7;
SELECT count(*) FROM tstu WHERE t = '5';
SELECT count(*) FROM tstu WHERE i = 7 AND t = '5';

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

-- Run amvalidator function on our opclasses
SELECT opcname, amvalidate(opc.oid)
FROM pg_opclass opc JOIN pg_am am ON am.oid = opcmethod
WHERE amname = 'bloom'
ORDER BY 1;

--
-- relation options
--
DROP INDEX bloomidx;
CREATE INDEX bloomidx ON tst USING bloom (i, t) WITH (length=7, col1=4);
SELECT reloptions FROM pg_class WHERE oid = 'bloomidx'::regclass;
-- check for min and max values
\set VERBOSITY terse
CREATE INDEX bloomidx2 ON tst USING bloom (i, t) WITH (length=0);
CREATE INDEX bloomidx2 ON tst USING bloom (i, t) WITH (col1=0);
