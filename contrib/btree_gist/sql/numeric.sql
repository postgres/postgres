-- numeric check

CREATE TABLE numerictmp (a numeric);

\copy numerictmp from 'data/int8.data'
\copy numerictmp from 'data/numeric.data'
\copy numerictmp from 'data/float8.data'

SET enable_seqscan=on;

SELECT count(*) FROM numerictmp WHERE a <  -1890.0;

SELECT count(*) FROM numerictmp WHERE a <= -1890.0;

SELECT count(*) FROM numerictmp WHERE a  = -1890.0;

SELECT count(*) FROM numerictmp WHERE a >= -1890.0;

SELECT count(*) FROM numerictmp WHERE a >  -1890.0;


SELECT count(*) FROM numerictmp WHERE a <  'NaN' ;

SELECT count(*) FROM numerictmp WHERE a <= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a  = 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >  'NaN' ;

SELECT count(*) FROM numerictmp WHERE a <  0 ;

SELECT count(*) FROM numerictmp WHERE a <= 0 ;

SELECT count(*) FROM numerictmp WHERE a  = 0 ;

SELECT count(*) FROM numerictmp WHERE a >= 0 ;

SELECT count(*) FROM numerictmp WHERE a >  0 ;


CREATE INDEX numericidx ON numerictmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM numerictmp WHERE a <  -1890.0;

SELECT count(*) FROM numerictmp WHERE a <= -1890.0;

SELECT count(*) FROM numerictmp WHERE a  = -1890.0;

SELECT count(*) FROM numerictmp WHERE a >= -1890.0;

SELECT count(*) FROM numerictmp WHERE a >  -1890.0;


SELECT count(*) FROM numerictmp WHERE a <  'NaN' ;

SELECT count(*) FROM numerictmp WHERE a <= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a  = 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >= 'NaN' ;

SELECT count(*) FROM numerictmp WHERE a >  'NaN' ;


SELECT count(*) FROM numerictmp WHERE a <  0 ;

SELECT count(*) FROM numerictmp WHERE a <= 0 ;

SELECT count(*) FROM numerictmp WHERE a  = 0 ;

SELECT count(*) FROM numerictmp WHERE a >= 0 ;

SELECT count(*) FROM numerictmp WHERE a >  0 ;
