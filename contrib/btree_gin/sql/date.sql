set enable_seqscan=off;

CREATE TABLE test_date (
	i date
);

INSERT INTO test_date VALUES 
	( '2004-10-23' ),
	( '2004-10-24' ),
	( '2004-10-25' ),
	( '2004-10-26' ),
	( '2004-10-27' ),
	( '2004-10-28' )
;

CREATE INDEX idx_date ON test_date USING gin (i);

SELECT * FROM test_date WHERE i<'2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i<='2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i='2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i>='2004-10-26'::date ORDER BY i;
SELECT * FROM test_date WHERE i>'2004-10-26'::date ORDER BY i;
