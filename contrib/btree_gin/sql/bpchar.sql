set enable_seqscan=off;

CREATE TABLE test_bpchar (
	i char(10)
);

INSERT INTO test_bpchar VALUES ('a'),('ab'),('abc'),('abc   '),('abb'),('axy'),('xyz'),('xyz  ');

CREATE INDEX idx_bpchar ON test_bpchar USING gin (i);

SELECT * FROM test_bpchar WHERE i<'abc' ORDER BY i;
SELECT * FROM test_bpchar WHERE i<='abc' ORDER BY i;
SELECT * FROM test_bpchar WHERE i='abc' ORDER BY i;
SELECT * FROM test_bpchar WHERE i='abc  ' ORDER BY i;
SELECT * FROM test_bpchar WHERE i>='abc' ORDER BY i;
SELECT * FROM test_bpchar WHERE i>'abc' ORDER BY i;

EXPLAIN (COSTS OFF) SELECT * FROM test_bpchar WHERE i<'abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_bpchar WHERE i<='abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_bpchar WHERE i='abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_bpchar WHERE i>='abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_bpchar WHERE i>'abc' ORDER BY i;
