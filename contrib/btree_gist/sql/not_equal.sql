
SET enable_seqscan to false;

-- test search for "not equals"

CREATE TABLE test_ne (
   a  TIMESTAMP,
   b  NUMERIC
);
CREATE INDEX test_ne_idx ON test_ne USING gist (a, b);

INSERT INTO test_ne SELECT '2009-01-01', 10.7 FROM generate_series(1,1000);
INSERT INTO test_ne VALUES('2007-02-03', -91.3);
INSERT INTO test_ne VALUES('2011-09-01', 43.7);
INSERT INTO test_ne SELECT '2009-01-01', 10.7 FROM generate_series(1,1000);

EXPLAIN (COSTS OFF) SELECT * FROM test_ne WHERE a <> '2009-01-01' AND b <> 10.7;

SELECT * FROM test_ne WHERE a <> '2009-01-01' AND b <> 10.7;

-- test search for "not equals" using an exclusion constraint

CREATE TABLE zoo (
   cage   INTEGER,
   animal TEXT,
   EXCLUDE USING gist (cage WITH =, animal WITH <>)
);

INSERT INTO zoo VALUES(123, 'zebra');
INSERT INTO zoo VALUES(123, 'zebra');
INSERT INTO zoo VALUES(123, 'lion');
INSERT INTO zoo VALUES(124, 'lion');
