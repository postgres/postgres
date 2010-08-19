-- int8 check

CREATE TABLE int8tmp (a int8);

\copy int8tmp from 'data/int8.data'

SET enable_seqscan=on;

SELECT count(*) FROM int8tmp WHERE a <  464571291354841;

SELECT count(*) FROM int8tmp WHERE a <= 464571291354841;

SELECT count(*) FROM int8tmp WHERE a  = 464571291354841;

SELECT count(*) FROM int8tmp WHERE a >= 464571291354841;

SELECT count(*) FROM int8tmp WHERE a >  464571291354841;

CREATE INDEX int8idx ON int8tmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM int8tmp WHERE a <  464571291354841::int8;

SELECT count(*) FROM int8tmp WHERE a <= 464571291354841::int8;

SELECT count(*) FROM int8tmp WHERE a  = 464571291354841::int8;

SELECT count(*) FROM int8tmp WHERE a >= 464571291354841::int8;

SELECT count(*) FROM int8tmp WHERE a >  464571291354841::int8;
