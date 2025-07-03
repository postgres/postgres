set enable_seqscan=off;

CREATE TABLE test_name (
	i name
);

INSERT INTO test_name VALUES ('a'),('ab'),('abc'),('abb'),('axy'),('xyz');

CREATE INDEX idx_name ON test_name USING gin (i);

SELECT * FROM test_name WHERE i<'abc' ORDER BY i;
SELECT * FROM test_name WHERE i<='abc' ORDER BY i;
SELECT * FROM test_name WHERE i='abc' ORDER BY i;
SELECT * FROM test_name WHERE i>='abc' ORDER BY i;
SELECT * FROM test_name WHERE i>'abc' ORDER BY i;

EXPLAIN (COSTS OFF) SELECT * FROM test_name WHERE i<'abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_name WHERE i<='abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_name WHERE i='abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_name WHERE i>='abc' ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_name WHERE i>'abc' ORDER BY i;

explain (costs off)
SELECT * FROM test_name WHERE i<'abc'::text ORDER BY i;

SELECT * FROM test_name WHERE i<'abc'::text ORDER BY i;
SELECT * FROM test_name WHERE i<='abc'::text ORDER BY i;
SELECT * FROM test_name WHERE i='abc'::text ORDER BY i;
SELECT * FROM test_name WHERE i>='abc'::text ORDER BY i;
SELECT * FROM test_name WHERE i>'abc'::text ORDER BY i;

SELECT * FROM test_name WHERE i<=repeat('abc', 100) ORDER BY i;
