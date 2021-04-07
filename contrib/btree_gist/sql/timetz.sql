-- timetz check

CREATE TABLE timetztmp (a timetz);
\copy timetztmp from 'data/timetz.data'

CREATE TABLE timetzcmp ( r_id int2, a int4, b int4 );


SET enable_seqscan=on;

INSERT INTO timetzcmp (r_id,a) SELECT  1,count(*) FROM timetztmp WHERE a <  '07:46:45 GMT+3';

INSERT INTO timetzcmp (r_id,a) SELECT  2,count(*) FROM timetztmp WHERE a <= '07:46:45 GMT+3';

INSERT INTO timetzcmp (r_id,a) SELECT  3,count(*) FROM timetztmp WHERE a  = '07:46:45 GMT+3';

INSERT INTO timetzcmp (r_id,a) SELECT  4,count(*) FROM timetztmp WHERE a >= '07:46:45 GMT+3';

INSERT INTO timetzcmp (r_id,a) SELECT  5,count(*) FROM timetztmp WHERE a >  '07:46:45 GMT+3';


INSERT INTO timetzcmp (r_id,a) SELECT 11,count(*) FROM timetztmp WHERE a <  '07:46:45 GMT+2';

INSERT INTO timetzcmp (r_id,a) SELECT 12,count(*) FROM timetztmp WHERE a <= '07:46:45 GMT+2';

INSERT INTO timetzcmp (r_id,a) SELECT 13,count(*) FROM timetztmp WHERE a  = '07:46:45 GMT+2';

INSERT INTO timetzcmp (r_id,a) SELECT 14,count(*) FROM timetztmp WHERE a >= '07:46:45 GMT+2';

INSERT INTO timetzcmp (r_id,a) SELECT 15,count(*) FROM timetztmp WHERE a >  '07:46:45 GMT+2';


INSERT INTO timetzcmp (r_id,a) SELECT 21,count(*) FROM timetztmp WHERE a <  '07:46:45 GMT+4';

INSERT INTO timetzcmp (r_id,a) SELECT 22,count(*) FROM timetztmp WHERE a <= '07:46:45 GMT+4';

INSERT INTO timetzcmp (r_id,a) SELECT 23,count(*) FROM timetztmp WHERE a  = '07:46:45 GMT+4';

INSERT INTO timetzcmp (r_id,a) SELECT 24,count(*) FROM timetztmp WHERE a >= '07:46:45 GMT+4';

INSERT INTO timetzcmp (r_id,a) SELECT 25,count(*) FROM timetztmp WHERE a >  '07:46:45 GMT+4';



SET client_min_messages = DEBUG1;
CREATE INDEX timetzidx ON timetztmp USING gist ( a );
CREATE INDEX timetzidx_b ON timetztmp USING gist ( a ) WITH (buffering=on);
DROP INDEX timetzidx_b;
RESET client_min_messages;

SET enable_seqscan=off;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a <  '07:46:45 GMT+3'::timetz ) q WHERE r_id=1 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a <= '07:46:45 GMT+3'::timetz ) q WHERE r_id=2 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a  = '07:46:45 GMT+3'::timetz ) q WHERE r_id=3 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a >= '07:46:45 GMT+3'::timetz ) q WHERE r_id=4 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a >  '07:46:45 GMT+3'::timetz ) q WHERE r_id=5 ;


UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a <  '07:46:45 GMT+2'::timetz ) q WHERE r_id=11 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a <= '07:46:45 GMT+2'::timetz ) q WHERE r_id=12 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a  = '07:46:45 GMT+2'::timetz ) q WHERE r_id=13 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a >= '07:46:45 GMT+2'::timetz ) q WHERE r_id=14 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a >  '07:46:45 GMT+2'::timetz ) q WHERE r_id=15 ;


UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a <  '07:46:45 GMT+4'::timetz ) q WHERE r_id=21 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a <= '07:46:45 GMT+4'::timetz ) q WHERE r_id=22 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a  = '07:46:45 GMT+4'::timetz ) q WHERE r_id=23 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a >= '07:46:45 GMT+4'::timetz ) q WHERE r_id=24 ;

UPDATE timetzcmp SET b=c FROM ( SELECT count(*) AS c FROM timetztmp WHERE a >  '07:46:45 GMT+4'::timetz ) q WHERE r_id=25 ;


SELECT count(*) FROM timetzcmp WHERE a=b;
