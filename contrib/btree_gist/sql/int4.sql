-- int4 check

CREATE TABLE int4tmp (a int4);

\copy int4tmp from 'data/int2.data'

SET enable_seqscan=on;

SELECT count(*) FROM int4tmp WHERE a <  237;

SELECT count(*) FROM int4tmp WHERE a <= 237;

SELECT count(*) FROM int4tmp WHERE a  = 237;

SELECT count(*) FROM int4tmp WHERE a >= 237;

SELECT count(*) FROM int4tmp WHERE a >  237;

CREATE INDEX int4idx ON int4tmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM int4tmp WHERE a <  237::int4;

SELECT count(*) FROM int4tmp WHERE a <= 237::int4;

SELECT count(*) FROM int4tmp WHERE a  = 237::int4;

SELECT count(*) FROM int4tmp WHERE a >= 237::int4;

SELECT count(*) FROM int4tmp WHERE a >  237::int4;
