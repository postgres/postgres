set enable_seqscan=off;

CREATE TABLE test_varchar (
	i varchar
);

INSERT INTO test_varchar VALUES ('a'),('ab'),('abc'),('abb'),('axy'),('xyz');

CREATE INDEX idx_varchar ON test_varchar USING gin (i);

SELECT * FROM test_varchar WHERE i<'abc'::varchar ORDER BY i;
SELECT * FROM test_varchar WHERE i<='abc'::varchar ORDER BY i;
SELECT * FROM test_varchar WHERE i='abc'::varchar ORDER BY i;
SELECT * FROM test_varchar WHERE i>='abc'::varchar ORDER BY i;
SELECT * FROM test_varchar WHERE i>'abc'::varchar ORDER BY i;
