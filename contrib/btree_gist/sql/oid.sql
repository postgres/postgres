-- oid check

SET enable_seqscan=on;

CREATE TEMPORARY TABLE oidtmp (oid oid);
INSERT INTO oidtmp SELECT g.i::oid FROM generate_series(1, 1000) g(i);

SELECT count(*) FROM oidtmp WHERE oid <  17;

SELECT count(*) FROM oidtmp WHERE oid <= 17;

SELECT count(*) FROM oidtmp WHERE oid  = 17;

SELECT count(*) FROM oidtmp WHERE oid >= 17;

SELECT count(*) FROM oidtmp WHERE oid >  17;

SET client_min_messages = DEBUG1;
CREATE INDEX oididx ON oidtmp USING gist ( oid );
CREATE INDEX oididx_b ON oidtmp USING gist ( oid ) WITH (buffering=on);
DROP INDEX oididx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM oidtmp WHERE oid <  17;

SELECT count(*) FROM oidtmp WHERE oid <= 17;

SELECT count(*) FROM oidtmp WHERE oid  = 17;

SELECT count(*) FROM oidtmp WHERE oid >= 17;

SELECT count(*) FROM oidtmp WHERE oid >  17;
