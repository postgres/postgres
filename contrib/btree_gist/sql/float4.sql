-- float4 check

CREATE TABLE float4tmp (a float4);

\copy float4tmp from 'data/float4.data'

SET enable_seqscan=on;

SELECT count(*) FROM float4tmp WHERE a <  -179.0;

SELECT count(*) FROM float4tmp WHERE a <= -179.0;

SELECT count(*) FROM float4tmp WHERE a  = -179.0;

SELECT count(*) FROM float4tmp WHERE a >= -179.0;

SELECT count(*) FROM float4tmp WHERE a >  -179.0;

SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;

CREATE INDEX float4idx ON float4tmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM float4tmp WHERE a <  -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a <= -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a  = -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a >= -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a >  -179.0::float4;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;
SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;

-- EXCLUDE constraint must block a duplicate NaN, same as it does for finite
-- values.
CREATE TABLE float4excl (a float4, EXCLUDE USING gist (a WITH =));
INSERT INTO float4excl VALUES ('NaN'::float4);
INSERT INTO float4excl VALUES ('NaN'::float4);  -- expect: violates EXCLUDE
SELECT count(*) FROM float4excl;

-- Test double-column index
CREATE INDEX float4idx2 ON float4tmp USING gist ( a, abs(a) );
EXPLAIN (COSTS OFF)
SELECT count(*) FROM float4tmp WHERE abs(a) = 179.0::float4;
SELECT count(*) FROM float4tmp WHERE abs(a) = 179.0::float4;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
