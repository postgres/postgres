-- char check

CREATE TABLE chartmp (a char(32));

\copy chartmp from 'data/char.data'

SET enable_seqscan=on;

SELECT count(*) FROM chartmp WHERE a <   '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a <=  '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a  =  '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a >=  '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a >   '31b0'::char(32);

CREATE INDEX charidx ON chartmp USING GIST ( a );

SET enable_seqscan=off;

SELECT count(*) FROM chartmp WHERE a <   '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a <=  '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a  =  '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a >=  '31b0'::char(32);

SELECT count(*) FROM chartmp WHERE a >   '31b0'::char(32);
