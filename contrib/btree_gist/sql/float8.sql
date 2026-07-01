-- float8 check

CREATE TABLE float8tmp (a float8);

\copy float8tmp from 'data/float8.data'

SET enable_seqscan=on;

SELECT count(*) FROM float8tmp WHERE a <  -1890.0;

SELECT count(*) FROM float8tmp WHERE a <= -1890.0;

SELECT count(*) FROM float8tmp WHERE a  = -1890.0;

SELECT count(*) FROM float8tmp WHERE a >= -1890.0;

SELECT count(*) FROM float8tmp WHERE a >  -1890.0;

SELECT a, a <-> '-1890.0' FROM float8tmp ORDER BY a <-> '-1890.0' LIMIT 3;

CREATE INDEX float8idx ON float8tmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM float8tmp WHERE a <  -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a <= -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a  = -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a >= -1890.0::float8;

SELECT count(*) FROM float8tmp WHERE a >  -1890.0::float8;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '-1890.0' FROM float8tmp ORDER BY a <-> '-1890.0' LIMIT 3;
SELECT a, a <-> '-1890.0' FROM float8tmp ORDER BY a <-> '-1890.0' LIMIT 3;

-- EXCLUDE constraint must block a duplicate NaN, same as it does for finite
-- values.
CREATE TABLE float8excl (a float8, EXCLUDE USING gist (a WITH =));
INSERT INTO float8excl VALUES ('NaN'::float8);
INSERT INTO float8excl VALUES ('NaN'::float8);  -- expect: violates EXCLUDE
SELECT count(*) FROM float8excl;

-- Test double-column index
CREATE INDEX float8idx2 ON float8tmp USING gist ( a, abs(a) );
EXPLAIN (COSTS OFF)
SELECT count(*) FROM float8tmp WHERE abs(a) = 1890.0::float8;
SELECT count(*) FROM float8tmp WHERE abs(a) = 1890.0::float8;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
