set enable_seqscan=off;

CREATE TABLE test_inet (
	i inet
);

INSERT INTO test_inet VALUES
	( '1.2.3.4/16' ),
	( '1.2.4.4/16' ),
	( '1.2.5.4/16' ),
	( '1.2.6.4/16' ),
	( '1.2.7.4/16' ),
	( '1.2.8.4/16' )
;

CREATE INDEX idx_inet ON test_inet USING gin (i);

SELECT * FROM test_inet WHERE i<'1.2.6.4/16'::inet ORDER BY i;
SELECT * FROM test_inet WHERE i<='1.2.6.4/16'::inet ORDER BY i;
SELECT * FROM test_inet WHERE i='1.2.6.4/16'::inet ORDER BY i;
SELECT * FROM test_inet WHERE i>='1.2.6.4/16'::inet ORDER BY i;
SELECT * FROM test_inet WHERE i>'1.2.6.4/16'::inet ORDER BY i;
