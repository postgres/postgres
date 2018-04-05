set enable_seqscan=off;

CREATE TABLE test_uuid (
	i uuid
);

INSERT INTO test_uuid VALUES
	( '00000000-0000-0000-0000-000000000000' ),
	( '299bc99f-2f79-4e3e-bfea-2cbfd62a7c27' ),
	( '6264af33-0d43-4337-bf4e-43509b8a4be8' ),
	( 'ce41c936-6acb-4feb-8c91-852a673e5a5c' ),
	( 'd2ce731f-f2a8-4a2b-be37-8f0ba637427f' ),
	( 'ffffffff-ffff-ffff-ffff-ffffffffffff' )
;

CREATE INDEX idx_uuid ON test_uuid USING gin (i);

SELECT * FROM test_uuid WHERE i<'ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
SELECT * FROM test_uuid WHERE i<='ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
SELECT * FROM test_uuid WHERE i='ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
SELECT * FROM test_uuid WHERE i>='ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
SELECT * FROM test_uuid WHERE i>'ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;

EXPLAIN (COSTS OFF) SELECT * FROM test_uuid WHERE i<'ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_uuid WHERE i<='ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_uuid WHERE i='ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_uuid WHERE i>='ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
EXPLAIN (COSTS OFF) SELECT * FROM test_uuid WHERE i>'ce41c936-6acb-4feb-8c91-852a673e5a5c'::uuid ORDER BY i;
