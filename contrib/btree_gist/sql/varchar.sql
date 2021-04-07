-- char check

CREATE TABLE vchartmp (a varchar(32));

\copy vchartmp from 'data/char.data'

SET enable_seqscan=on;

SELECT count(*) FROM vchartmp WHERE a <   '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a <=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a  =  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >   '31b0'::varchar(32);

SET client_min_messages = DEBUG1;
CREATE INDEX vcharidx ON vchartmp USING GIST ( text(a) );
CREATE INDEX vcharidx_b ON vchartmp USING GIST ( text(a) ) WITH (buffering=on);
DROP INDEX vcharidx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM vchartmp WHERE a <   '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a <=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a  =  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >=  '31b0'::varchar(32);

SELECT count(*) FROM vchartmp WHERE a >   '31b0'::varchar(32);
