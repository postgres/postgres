set enable_seqscan=off;

CREATE TABLE test_varbit (
	i varbit
);

INSERT INTO test_varbit VALUES ('001'),('010'),('011'),('100'),('101'),('110');

CREATE INDEX idx_varbit ON test_varbit USING gin (i);

SELECT * FROM test_varbit WHERE i<'100'::varbit ORDER BY i;
SELECT * FROM test_varbit WHERE i<='100'::varbit ORDER BY i;
SELECT * FROM test_varbit WHERE i='100'::varbit ORDER BY i;
SELECT * FROM test_varbit WHERE i>='100'::varbit ORDER BY i;
SELECT * FROM test_varbit WHERE i>'100'::varbit ORDER BY i;
