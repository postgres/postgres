-- oid check

SET enable_seqscan=on;

SELECT count(*) FROM moneytmp WHERE oid <  ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid <= ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid  = ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid >= ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid >  ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

CREATE INDEX oididx ON moneytmp USING gist ( oid );

SET enable_seqscan=off;

SELECT count(*) FROM moneytmp WHERE oid <  ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid <= ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid  = ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid >= ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );

SELECT count(*) FROM moneytmp WHERE oid >  ( SELECT oid FROM moneytmp WHERE a  = '22649.64' );
