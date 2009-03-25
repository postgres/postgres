set enable_seqscan=off;

CREATE TABLE test_oid (
	i oid
);

INSERT INTO test_oid VALUES (0),(1),(2),(3),(4),(5);

CREATE INDEX idx_oid ON test_oid USING gin (i);

SELECT * FROM test_oid WHERE i<3::oid ORDER BY i;
SELECT * FROM test_oid WHERE i<=3::oid ORDER BY i;
SELECT * FROM test_oid WHERE i=3::oid ORDER BY i;
SELECT * FROM test_oid WHERE i>=3::oid ORDER BY i;
SELECT * FROM test_oid WHERE i>3::oid ORDER BY i;
