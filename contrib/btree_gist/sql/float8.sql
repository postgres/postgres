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
