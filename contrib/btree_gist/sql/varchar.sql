-- char check

CREATE TABLE vchartmp (a varchar(32));

\copy vchartmp from 'data/char.data'

SET enable_seqscan=on;

SELECT count(*) FROM vchartmp WHERE a <   '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a <=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a  =  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >   '31b0'::varchar(32);

CREATE INDEX vcharidx ON vchartmp USING GIST ( text(a) );

SET enable_seqscan=off;

SELECT count(*) FROM vchartmp WHERE a <   '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a <=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a  =  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >   '31b0'::varchar(32);
