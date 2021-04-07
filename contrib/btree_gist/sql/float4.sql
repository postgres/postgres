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

SET client_min_messages = DEBUG1;
CREATE INDEX float4idx ON float4tmp USING gist ( a );
CREATE INDEX float4idx_b ON float4tmp USING gist ( a ) WITH (buffering=on);
DROP INDEX float4idx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM float4tmp WHERE a <  -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a <= -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a  = -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a >= -179.0::float4;

SELECT count(*) FROM float4tmp WHERE a >  -179.0::float4;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;
SELECT a, a <-> '-179.0' FROM float4tmp ORDER BY a <-> '-179.0' LIMIT 3;
