-- timestamptz check

CREATE TABLE timestamptztmp (a timestamptz);

\copy timestamptztmp from 'data/timestamptz.data'

SET enable_seqscan=on;

SELECT count(*) FROM timestamptztmp WHERE a <  '2018-12-18 10:59:54 GMT+3';

SELECT count(*) FROM timestamptztmp WHERE a <= '2018-12-18 10:59:54 GMT+3';

SELECT count(*) FROM timestamptztmp WHERE a  = '2018-12-18 10:59:54 GMT+3';

SELECT count(*) FROM timestamptztmp WHERE a >= '2018-12-18 10:59:54 GMT+3';

SELECT count(*) FROM timestamptztmp WHERE a >  '2018-12-18 10:59:54 GMT+3';


SELECT count(*) FROM timestamptztmp WHERE a <  '2018-12-18 10:59:54 GMT+2';

SELECT count(*) FROM timestamptztmp WHERE a <= '2018-12-18 10:59:54 GMT+2';

SELECT count(*) FROM timestamptztmp WHERE a  = '2018-12-18 10:59:54 GMT+2';

SELECT count(*) FROM timestamptztmp WHERE a >= '2018-12-18 10:59:54 GMT+2';

SELECT count(*) FROM timestamptztmp WHERE a >  '2018-12-18 10:59:54 GMT+2';

SELECT count(*) FROM timestamptztmp WHERE a <  '2018-12-18 10:59:54 GMT+4';

SELECT count(*) FROM timestamptztmp WHERE a <= '2018-12-18 10:59:54 GMT+4';

SELECT count(*) FROM timestamptztmp WHERE a  = '2018-12-18 10:59:54 GMT+4';

SELECT count(*) FROM timestamptztmp WHERE a >= '2018-12-18 10:59:54 GMT+4';

SELECT count(*) FROM timestamptztmp WHERE a >  '2018-12-18 10:59:54 GMT+4';



CREATE INDEX timestamptzidx ON timestamptztmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM timestamptztmp WHERE a <  '2018-12-18 10:59:54 GMT+3'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a <= '2018-12-18 10:59:54 GMT+3'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a  = '2018-12-18 10:59:54 GMT+3'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a >= '2018-12-18 10:59:54 GMT+3'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a >  '2018-12-18 10:59:54 GMT+3'::timestamptz;


SELECT count(*) FROM timestamptztmp WHERE a <  '2018-12-18 10:59:54 GMT+2'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a <= '2018-12-18 10:59:54 GMT+2'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a  = '2018-12-18 10:59:54 GMT+2'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a >= '2018-12-18 10:59:54 GMT+2'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a >  '2018-12-18 10:59:54 GMT+2'::timestamptz;


SELECT count(*) FROM timestamptztmp WHERE a <  '2018-12-18 10:59:54 GMT+4'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a <= '2018-12-18 10:59:54 GMT+4'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a  = '2018-12-18 10:59:54 GMT+4'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a >= '2018-12-18 10:59:54 GMT+4'::timestamptz;

SELECT count(*) FROM timestamptztmp WHERE a >  '2018-12-18 10:59:54 GMT+4'::timestamptz;
