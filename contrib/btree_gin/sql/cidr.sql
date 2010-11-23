set enable_seqscan=off;

CREATE TABLE test_cidr (
	i cidr
);

INSERT INTO test_cidr VALUES
	( '1.2.3.4' ),
	( '1.2.4.4' ),
	( '1.2.5.4' ),
	( '1.2.6.4' ),
	( '1.2.7.4' ),
	( '1.2.8.4' )
;

CREATE INDEX idx_cidr ON test_cidr USING gin (i);

SELECT * FROM test_cidr WHERE i<'1.2.6.4'::cidr ORDER BY i;
SELECT * FROM test_cidr WHERE i<='1.2.6.4'::cidr ORDER BY i;
SELECT * FROM test_cidr WHERE i='1.2.6.4'::cidr ORDER BY i;
SELECT * FROM test_cidr WHERE i>='1.2.6.4'::cidr ORDER BY i;
SELECT * FROM test_cidr WHERE i>'1.2.6.4'::cidr ORDER BY i;
