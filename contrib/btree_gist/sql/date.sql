-- date check

CREATE TABLE datetmp (a date);

\copy datetmp from 'data/date.data'

SET enable_seqscan=on;

SELECT count(*) FROM datetmp WHERE a <  '2001-02-13';

SELECT count(*) FROM datetmp WHERE a <= '2001-02-13';

SELECT count(*) FROM datetmp WHERE a  = '2001-02-13';

SELECT count(*) FROM datetmp WHERE a >= '2001-02-13';

SELECT count(*) FROM datetmp WHERE a >  '2001-02-13';

SELECT a, a <-> '2001-02-13' FROM datetmp ORDER BY a <-> '2001-02-13' LIMIT 3;

CREATE INDEX dateidx ON datetmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM datetmp WHERE a <  '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a <= '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a  = '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a >= '2001-02-13'::date;

SELECT count(*) FROM datetmp WHERE a >  '2001-02-13'::date;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '2001-02-13' FROM datetmp ORDER BY a <-> '2001-02-13' LIMIT 3;
SELECT a, a <-> '2001-02-13' FROM datetmp ORDER BY a <-> '2001-02-13' LIMIT 3;

-- infinite dates in an index with internal pages (penalty and KNN distance)
CREATE TABLE dateinf (a date);
CREATE INDEX dateinfidx ON dateinf USING gist (a);
INSERT INTO dateinf SELECT '2000-01-01'::date + i FROM generate_series(1, 1000) i;
INSERT INTO dateinf SELECT 'infinity' FROM generate_series(1, 3);
INSERT INTO dateinf SELECT '-infinity' FROM generate_series(1, 3);
EXPLAIN (COSTS OFF)
SELECT a FROM dateinf ORDER BY a <-> '2000-01-01'::date LIMIT 3;
SELECT a FROM dateinf ORDER BY a <-> '2000-01-01'::date LIMIT 3;
SELECT a FROM dateinf ORDER BY a <-> 'infinity'::date LIMIT 3;
SELECT a FROM dateinf ORDER BY a <-> '-infinity'::date LIMIT 3;
-- same orderings via sort, which evaluates the <-> operator on every row
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF)
SELECT a FROM dateinf ORDER BY a <-> '2000-01-01'::date LIMIT 3;
SELECT a FROM dateinf ORDER BY a <-> '2000-01-01'::date LIMIT 3;
SELECT a FROM dateinf ORDER BY a <-> 'infinity'::date LIMIT 3;
SELECT a FROM dateinf ORDER BY a <-> '-infinity'::date LIMIT 3;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
-- and check these distance results directly
SELECT 'infinity'::date <-> 'infinity'::date, '-infinity'::date <-> '-infinity'::date,
       'infinity'::date <-> '-infinity'::date, 'infinity'::date <-> '2000-01-01'::date;
