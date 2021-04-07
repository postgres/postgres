-- enum check

create type rainbow as enum ('r','o','y','g','b','i','v');

CREATE TABLE enumtmp (a rainbow);

\copy enumtmp from 'data/enum.data'

SET enable_seqscan=on;

select a, count(*) from enumtmp group by a order by 1;

SELECT count(*) FROM enumtmp WHERE a <  'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a <= 'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a  = 'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a >= 'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a >  'g'::rainbow;

CREATE INDEX enumidx ON enumtmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM enumtmp WHERE a <  'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a <= 'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a  = 'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a >= 'g'::rainbow;

SELECT count(*) FROM enumtmp WHERE a >  'g'::rainbow;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM enumtmp WHERE a >= 'g'::rainbow;
