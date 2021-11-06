-- bool check

CREATE TABLE booltmp (a bool);

INSERT INTO booltmp VALUES (false), (true);

SET enable_seqscan=on;

SELECT count(*) FROM booltmp WHERE a <  true;

SELECT count(*) FROM booltmp WHERE a <= true;

SELECT count(*) FROM booltmp WHERE a  = true;

SELECT count(*) FROM booltmp WHERE a >= true;

SELECT count(*) FROM booltmp WHERE a >  true;

CREATE INDEX boolidx ON booltmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM booltmp WHERE a <  true;

SELECT count(*) FROM booltmp WHERE a <= true;

SELECT count(*) FROM booltmp WHERE a  = true;

SELECT count(*) FROM booltmp WHERE a >= true;

SELECT count(*) FROM booltmp WHERE a >  true;

-- Test index-only scans
SET enable_bitmapscan=off;

EXPLAIN (COSTS OFF)
SELECT * FROM booltmp WHERE a;
SELECT * FROM booltmp WHERE a;

EXPLAIN (COSTS OFF)
SELECT * FROM booltmp WHERE NOT a;
SELECT * FROM booltmp WHERE NOT a;
