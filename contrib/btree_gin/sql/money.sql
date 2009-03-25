set enable_seqscan=off;

CREATE TABLE test_money (
	i money
);

INSERT INTO test_money VALUES ('-2'),('-1'),('0'),('1'),('2'),('3');

CREATE INDEX idx_money ON test_money USING gin (i);

SELECT * FROM test_money WHERE i<'1'::money ORDER BY i;
SELECT * FROM test_money WHERE i<='1'::money ORDER BY i;
SELECT * FROM test_money WHERE i='1'::money ORDER BY i;
SELECT * FROM test_money WHERE i>='1'::money ORDER BY i;
SELECT * FROM test_money WHERE i>'1'::money ORDER BY i;
