-- Tests for extensible node and custom scan

CREATE EXTENSION test_extensible;

SELECT test_get_extensible_node_methods('TestExtNode', false);
SELECT test_get_extensible_node_methods('NoSuchExtNode', true);
SELECT test_get_extensible_node_methods('NoSuchExtNode', false); -- error
SELECT test_get_custom_scan_methods('TestCustomScan', false);
SELECT test_get_custom_scan_methods('NoSuchCustomScan', true);
SELECT test_get_custom_scan_methods('NoSuchCustomScan', false); -- error

-- STRICT checks
SELECT test_get_extensible_node_methods(NULL, false) IS NULL AS null_name;
SELECT test_get_extensible_node_methods('TestExtNode', NULL) IS NULL AS null_missing_ok;
SELECT test_get_custom_scan_methods(NULL, false) IS NULL AS null_name;
SELECT test_get_custom_scan_methods('TestCustomScan', NULL) IS NULL AS null_missing_ok;

-- callbacks checks: node read, copy, equality.
SELECT test_ext_node_make('1234'::oid, 2);
-- node read restores information written
SELECT test_ext_node_get_relid(n) AS relid,
       test_ext_node_get_repeat_count(n) AS repeat_count
  FROM (SELECT test_ext_node_make('1234'::oid, 2)) AS s(n);
-- node copy
SELECT test_ext_node_equal(n, test_ext_node_copy(n)) AS copy_is_equal,
       test_ext_node_copy(n) = n AS copy_is_identical
  FROM (SELECT test_ext_node_make('1234'::oid, 2)) AS s(n);
-- node equality
SELECT test_ext_node_equal(test_ext_node_make('1234'::oid, 2),
                           test_ext_node_make('1234'::oid, 3)) AS other_repeat_count,
       test_ext_node_equal(test_ext_node_make('1234'::oid, 2),
                           test_ext_node_make('5678'::oid, 2)) AS other_relid;

-- correct node type, missing field
SELECT test_ext_node_get_relid('{EXTENSIBLENODE :extnodename TestExtNode}');

-- CustomScan tests
CREATE TABLE test_extensible_tbl (id integer, val text);
INSERT INTO test_extensible_tbl VALUES (1, 'one'), (2, 'two'), (3, 'three');

-- Planner picks a CustomScan over a SeqScan
EXPLAIN (COSTS OFF) SELECT id, val FROM test_extensible_tbl ORDER BY id;
SELECT id, val FROM test_extensible_tbl ORDER BY id;

-- Restriction clauses
EXPLAIN (COSTS OFF) SELECT val FROM test_extensible_tbl WHERE id > 1;
SELECT val FROM test_extensible_tbl WHERE id > 1 ORDER BY id;

-- repeat_count is read at plan time, so a later SET has no effect on it
SET test_extensible.repeat_count = 3;
SELECT id, val FROM test_extensible_tbl ORDER BY id;
RESET test_extensible.repeat_count;

-- TABLESAMPLE is uses a core scan
EXPLAIN (COSTS OFF) SELECT id FROM test_extensible_tbl TABLESAMPLE SYSTEM (0);
SELECT count(*) FROM test_extensible_tbl TABLESAMPLE SYSTEM (0);

-- CustomScan below a Gather
SET max_parallel_workers_per_gather = 2;
SET debug_parallel_query = on;
EXPLAIN (COSTS OFF) SELECT id, val FROM test_extensible_tbl ORDER BY id;

-- regress mode hides the Gather, so the plan matches the serial one above
SET debug_parallel_query = regress;
EXPLAIN (COSTS OFF) SELECT id, val FROM test_extensible_tbl ORDER BY id;
SELECT id, val FROM test_extensible_tbl ORDER BY id;
RESET debug_parallel_query;
RESET max_parallel_workers_per_gather;

-- inheritance: a parent stands for the whole hierarchy
CREATE TABLE test_extensible_child () INHERITS (test_extensible_tbl);
INSERT INTO test_extensible_child VALUES (4, 'four');
EXPLAIN (COSTS OFF) SELECT id, val FROM test_extensible_tbl;
SELECT id, val FROM test_extensible_tbl ORDER BY id;

-- ONLY case, parent only.
EXPLAIN (COSTS OFF) SELECT id, val FROM ONLY test_extensible_tbl;
SELECT id, val FROM ONLY test_extensible_tbl ORDER BY id;
DROP TABLE test_extensible_child;

DROP TABLE test_extensible_tbl;

DROP EXTENSION test_extensible;
