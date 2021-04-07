-- int2 check

CREATE TABLE int2tmp (a int2);

\copy int2tmp from 'data/int2.data'

SET enable_seqscan=on;

SELECT count(*) FROM int2tmp WHERE a <  237;

SELECT count(*) FROM int2tmp WHERE a <= 237;

SELECT count(*) FROM int2tmp WHERE a  = 237;

SELECT count(*) FROM int2tmp WHERE a >= 237;

SELECT count(*) FROM int2tmp WHERE a >  237;

SELECT a, a <-> '237' FROM int2tmp ORDER BY a <-> '237' LIMIT 3;

SET client_min_messages = DEBUG1;
CREATE INDEX int2idx ON int2tmp USING gist ( a );
CREATE INDEX int2idx_b ON int2tmp USING gist ( a ) WITH (buffering=on);
DROP INDEX int2idx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM int2tmp WHERE a <  237::int2;

SELECT count(*) FROM int2tmp WHERE a <= 237::int2;

SELECT count(*) FROM int2tmp WHERE a  = 237::int2;

SELECT count(*) FROM int2tmp WHERE a >= 237::int2;

SELECT count(*) FROM int2tmp WHERE a >  237::int2;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '237' FROM int2tmp ORDER BY a <-> '237' LIMIT 3;
SELECT a, a <-> '237' FROM int2tmp ORDER BY a <-> '237' LIMIT 3;
