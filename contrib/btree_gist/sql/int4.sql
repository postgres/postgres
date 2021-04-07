-- int4 check

CREATE TABLE int4tmp (a int4);

\copy int4tmp from 'data/int2.data'

SET enable_seqscan=on;

SELECT count(*) FROM int4tmp WHERE a <  237;

SELECT count(*) FROM int4tmp WHERE a <= 237;

SELECT count(*) FROM int4tmp WHERE a  = 237;

SELECT count(*) FROM int4tmp WHERE a >= 237;

SELECT count(*) FROM int4tmp WHERE a >  237;

SELECT a, a <-> '237' FROM int4tmp ORDER BY a <-> '237' LIMIT 3;

SET client_min_messages = DEBUG1;
CREATE INDEX int4idx ON int4tmp USING gist ( a );
CREATE INDEX int4idx_b ON int4tmp USING gist ( a ) WITH (buffering=on);
DROP INDEX int4idx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM int4tmp WHERE a <  237::int4;

SELECT count(*) FROM int4tmp WHERE a <= 237::int4;

SELECT count(*) FROM int4tmp WHERE a  = 237::int4;

SELECT count(*) FROM int4tmp WHERE a >= 237::int4;

SELECT count(*) FROM int4tmp WHERE a >  237::int4;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '237' FROM int4tmp ORDER BY a <-> '237' LIMIT 3;
SELECT a, a <-> '237' FROM int4tmp ORDER BY a <-> '237' LIMIT 3;
