-- time check

CREATE TABLE timetmp (a time);

\copy timetmp from 'data/time.data'

SET enable_seqscan=on;

SELECT count(*) FROM timetmp WHERE a <  '10:57:11';

SELECT count(*) FROM timetmp WHERE a <= '10:57:11';

SELECT count(*) FROM timetmp WHERE a  = '10:57:11';

SELECT count(*) FROM timetmp WHERE a >= '10:57:11';

SELECT count(*) FROM timetmp WHERE a >  '10:57:11';

CREATE INDEX timeidx ON timetmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM timetmp WHERE a <  '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a <= '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a  = '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a >= '10:57:11'::time;

SELECT count(*) FROM timetmp WHERE a >  '10:57:11'::time;

