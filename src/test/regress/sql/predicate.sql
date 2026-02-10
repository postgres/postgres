--
-- Tests for predicate handling
--

--
-- Test that restrictions that are always true are ignored, and that are always
-- false are replaced with constant-FALSE
--
-- Currently we only check for NullTest quals and OR clauses that include
-- NullTest quals.  We may extend it in the future.
--
CREATE TABLE pred_tab (a int NOT NULL, b int, c int NOT NULL);

--
-- Test restriction clauses
--

-- Ensure the IS_NOT_NULL qual is ignored when the column is non-nullable
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.a IS NOT NULL;

-- Ensure the IS_NOT_NULL qual is not ignored on a nullable column
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.b IS NOT NULL;

-- Ensure the IS_NULL qual is reduced to constant-FALSE for non-nullable
-- columns
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.a IS NULL;

-- Ensure the IS_NULL qual is not reduced to constant-FALSE on nullable
-- columns
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.b IS NULL;

--
-- Tests for OR clauses in restriction clauses
--

-- Ensure the OR clause is ignored when an OR branch is always true
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.a IS NOT NULL OR t.b = 1;

-- Ensure the OR clause is not ignored for NullTests that can't be proven
-- always true
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.b IS NOT NULL OR t.a = 1;

-- Ensure the OR clause is reduced to constant-FALSE when all branches are
-- provably false
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.a IS NULL OR t.c IS NULL;

-- Ensure the OR clause is not reduced to constant-FALSE when not all branches
-- are provably false
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t WHERE t.b IS NULL OR t.c IS NULL;

--
-- Test join clauses
--

-- Ensure the IS_NOT_NULL qual is ignored, since a) it's on a NOT NULL column,
-- and b) its Var is not nullable by any outer joins
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON t1.a IS NOT NULL;

-- Ensure the IS_NOT_NULL qual is not ignored when columns are made nullable
-- by an outer join
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    FULL JOIN pred_tab t2 ON t1.a = t2.a
    LEFT JOIN pred_tab t3 ON t2.a IS NOT NULL;

-- Ensure the IS_NULL qual is reduced to constant-FALSE, since a) it's on a NOT
-- NULL column, and b) its Var is not nullable by any outer joins
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON t1.a IS NULL;

-- Ensure the IS_NULL qual is not reduced to constant-FALSE when the column is
-- nullable by an outer join
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON t1.a = 1
    LEFT JOIN pred_tab t3 ON t2.a IS NULL;

--
-- Tests for OR clauses in join clauses
--

-- Ensure the OR clause is ignored when an OR branch is provably always true
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON t1.a IS NOT NULL OR t2.b = 1;

-- Ensure the NullTest is not ignored when the column is nullable by an outer
-- join
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    FULL JOIN pred_tab t2 ON t1.a = t2.a
    LEFT JOIN pred_tab t3 ON t2.a IS NOT NULL OR t2.b = 1;

-- Ensure the OR clause is reduced to constant-FALSE when all OR branches are
-- provably false
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON (t1.a IS NULL OR t1.c IS NULL);

-- Ensure the OR clause is not reduced to constant-FALSE when a column is
-- made nullable from an outer join
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON t1.a = 1
    LEFT JOIN pred_tab t3 ON t2.a IS NULL OR t2.c IS NULL;

--
-- Tests for NullTest reduction in EXISTS sublink
--

-- Ensure the IS_NOT_NULL qual is ignored
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON EXISTS
        (SELECT 1 FROM pred_tab t3, pred_tab t4, pred_tab t5, pred_tab t6
         WHERE t1.a = t3.a AND t6.a IS NOT NULL);

-- Ensure the IS_NULL qual is reduced to constant-FALSE
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON EXISTS
        (SELECT 1 FROM pred_tab t3, pred_tab t4, pred_tab t5, pred_tab t6
         WHERE t1.a = t3.a AND t6.a IS NULL);

DROP TABLE pred_tab;

-- Validate we handle IS NULL and IS NOT NULL quals correctly with inheritance
-- parents.
CREATE TABLE pred_parent (a int);
CREATE TABLE pred_child () INHERITS (pred_parent);
ALTER TABLE ONLY pred_parent ALTER a SET NOT NULL;

-- Ensure that the scan on pred_child contains the IS NOT NULL qual.
EXPLAIN (COSTS OFF)
SELECT * FROM pred_parent WHERE a IS NOT NULL;

-- Ensure we only scan pred_child and not pred_parent
EXPLAIN (COSTS OFF)
SELECT * FROM pred_parent WHERE a IS NULL;

ALTER TABLE pred_parent ALTER a DROP NOT NULL;
ALTER TABLE pred_child ALTER a SET NOT NULL;

-- Ensure the IS NOT NULL qual is removed from the pred_child scan.
EXPLAIN (COSTS OFF)
SELECT * FROM pred_parent WHERE a IS NOT NULL;

-- Ensure we only scan pred_parent and not pred_child
EXPLAIN (COSTS OFF)
SELECT * FROM pred_parent WHERE a IS NULL;

DROP TABLE pred_parent, pred_child;

-- Validate we do not reduce a clone clause to a constant true or false
CREATE TABLE pred_tab (a int, b int);
CREATE TABLE pred_tab_notnull (a int, b int NOT NULL);

INSERT INTO pred_tab VALUES (1, 1);
INSERT INTO pred_tab VALUES (2, 2);

INSERT INTO pred_tab_notnull VALUES (2, 2);
INSERT INTO pred_tab_notnull VALUES (3, 3);

ANALYZE pred_tab;
ANALYZE pred_tab_notnull;

-- Ensure the IS_NOT_NULL qual is not reduced to constant true and removed
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON TRUE
    LEFT JOIN pred_tab_notnull t3 ON t2.a = t3.a
    LEFT JOIN pred_tab t4 ON t3.b IS NOT NULL;

SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON TRUE
    LEFT JOIN pred_tab_notnull t3 ON t2.a = t3.a
    LEFT JOIN pred_tab t4 ON t3.b IS NOT NULL;

-- Ensure the IS_NULL qual is not reduced to constant false
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON TRUE
    LEFT JOIN pred_tab_notnull t3 ON t2.a = t3.a
    LEFT JOIN pred_tab t4 ON t3.b IS NULL AND t3.a IS NOT NULL;

SELECT * FROM pred_tab t1
    LEFT JOIN pred_tab t2 ON TRUE
    LEFT JOIN pred_tab_notnull t3 ON t2.a = t3.a
    LEFT JOIN pred_tab t4 ON t3.b IS NULL AND t3.a IS NOT NULL;

DROP TABLE pred_tab;
DROP TABLE pred_tab_notnull;

-- Validate that NullTest quals in constraint expressions are reduced correctly
CREATE TABLE pred_tab1 (a int NOT NULL, b int,
	CONSTRAINT check_tab1 CHECK (a IS NULL OR b > 2));
CREATE TABLE pred_tab2 (a int, b int,
	CONSTRAINT check_a CHECK (a IS NOT NULL));

SET constraint_exclusion TO ON;

-- Ensure that we get a dummy plan
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab1, pred_tab2 WHERE pred_tab2.a IS NULL;

-- Ensure that we get a dummy plan
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab2, pred_tab1 WHERE pred_tab1.a IS NULL OR pred_tab1.b < 2;

RESET constraint_exclusion;
DROP TABLE pred_tab1;
DROP TABLE pred_tab2;

-- Validate that NullTest quals in index expressions and predicate are reduced correctly
CREATE TABLE pred_tab (a int, b int NOT NULL, c int NOT NULL);
INSERT INTO pred_tab SELECT i, i, i FROM generate_series(1, 1000) i;
CREATE INDEX pred_tab_exprs_idx ON pred_tab ((a < 5 AND b IS NOT NULL AND c IS NOT NULL));
CREATE INDEX pred_tab_pred_idx ON pred_tab (a) WHERE b IS NOT NULL AND c IS NOT NULL;
ANALYZE pred_tab;

-- Ensure that index pred_tab_exprs_idx is used
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (a < 5 AND b IS NOT NULL AND c IS NOT NULL) IS TRUE;
SELECT * FROM pred_tab WHERE (a < 5 AND b IS NOT NULL AND c IS NOT NULL) IS TRUE;

-- Ensure that index pred_tab_pred_idx is used
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE a < 3 AND b IS NOT NULL AND c IS NOT NULL;
SELECT * FROM pred_tab WHERE a < 3 AND b IS NOT NULL AND c IS NOT NULL;

DROP TABLE pred_tab;

--
-- Test that COALESCE expressions in predicates are simplified using
-- non-nullable arguments.
--
CREATE TABLE pred_tab (a int NOT NULL, b int, c int);

-- Ensure that constant NULL arguments are dropped
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE COALESCE(NULL, b, NULL, a) > 1;

-- Ensure that argument "b*a" is dropped
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE COALESCE(b, a, b*a) > 1;

-- Ensure that the entire COALESCE expression is replaced by "a"
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE COALESCE(a, b) > 1;

--
-- Test detection of non-nullable expressions in predicates
--

-- CoalesceExpr
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE COALESCE(b, a) IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE COALESCE(b, c) IS NULL;

-- MinMaxExpr
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE GREATEST(b, a) IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE GREATEST(b, c) IS NULL;

-- CaseExpr
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (CASE WHEN c > 0 THEN a ELSE a END) IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (CASE WHEN c > 0 THEN b ELSE a END) IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (CASE WHEN c > 0 THEN a END) IS NULL;

-- ArrayExpr
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE ARRAY[b] IS NULL;

-- NullTest
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (b IS NULL) IS NULL;

-- BooleanTest
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE ((a > 1) IS TRUE) IS NULL;

-- DistinctExpr
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (a IS DISTINCT FROM b) IS NULL;

-- RelabelType
EXPLAIN (COSTS OFF)
SELECT * FROM pred_tab WHERE (a::oid) IS NULL;

DROP TABLE pred_tab;

--
-- Test optimization of IS [NOT] DISTINCT FROM
--

CREATE TYPE dist_row_t AS (a int, b int);
CREATE TABLE dist_tab (id int, val_nn int NOT NULL, val_null int, row_nn dist_row_t NOT NULL);

INSERT INTO dist_tab VALUES (1, 10, 10, ROW(1, 1));
INSERT INTO dist_tab VALUES (2, 20, NULL, ROW(2, 2));
INSERT INTO dist_tab VALUES (3, 30, 30, ROW(1, NULL));

CREATE INDEX dist_tab_nn_idx ON dist_tab (val_nn);

ANALYZE dist_tab;

-- Ensure that the predicate folds to constant TRUE
EXPLAIN(COSTS OFF)
SELECT id FROM dist_tab WHERE val_nn IS DISTINCT FROM NULL::INT;
SELECT id FROM dist_tab WHERE val_nn IS DISTINCT FROM NULL::INT;

-- Ensure that the predicate folds to constant FALSE
EXPLAIN(COSTS OFF)
SELECT id FROM dist_tab WHERE val_nn IS NOT DISTINCT FROM NULL::INT;
SELECT id FROM dist_tab WHERE val_nn IS NOT DISTINCT FROM NULL::INT;

-- Ensure that the predicate is converted to an inequality operator
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE val_nn IS DISTINCT FROM 10;
SELECT id FROM dist_tab WHERE val_nn IS DISTINCT FROM 10;

-- Ensure that the predicate is converted to an equality operator, and thus can
-- use index scan
SET enable_seqscan TO off;
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE val_nn IS NOT DISTINCT FROM 10;
SELECT id FROM dist_tab WHERE val_nn IS NOT DISTINCT FROM 10;
RESET enable_seqscan;

-- Ensure that the predicate is preserved as "IS DISTINCT FROM"
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE val_null IS DISTINCT FROM 20;
SELECT id FROM dist_tab WHERE val_null IS DISTINCT FROM 20;

-- Safety check for rowtypes
-- Ensure that the predicate is converted to an inequality operator
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE row_nn IS DISTINCT FROM ROW(1, 5)::dist_row_t;
-- ... and that all 3 rows are returned
SELECT id FROM dist_tab WHERE row_nn IS DISTINCT FROM ROW(1, 5)::dist_row_t;

-- Ensure that the predicate is converted to an equality operator, and thus
-- mergejoinable or hashjoinable
SET enable_nestloop TO off;
EXPLAIN (COSTS OFF)
SELECT * FROM dist_tab t1 JOIN dist_tab t2 ON t1.val_nn IS NOT DISTINCT FROM t2.val_nn;
SELECT * FROM dist_tab t1 JOIN dist_tab t2 ON t1.val_nn IS NOT DISTINCT FROM t2.val_nn;
RESET enable_nestloop;

-- Ensure that the predicate is converted to IS NOT NULL
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE val_null IS DISTINCT FROM NULL::INT;
SELECT id FROM dist_tab WHERE val_null IS DISTINCT FROM NULL::INT;

-- Ensure that the predicate is converted to IS NULL
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE val_null IS NOT DISTINCT FROM NULL::INT;
SELECT id FROM dist_tab WHERE val_null IS NOT DISTINCT FROM NULL::INT;

-- Safety check for rowtypes
-- The predicate is converted to IS NOT NULL, and get_rule_expr prints it as IS
-- DISTINCT FROM because argisrow is false, indicating that we're applying a
-- scalar test
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE (val_null, val_null) IS DISTINCT FROM NULL::RECORD;
SELECT id FROM dist_tab WHERE (val_null, val_null) IS DISTINCT FROM NULL::RECORD;

-- The predicate is converted to IS NULL, and get_rule_expr prints it as IS NOT
-- DISTINCT FROM because argisrow is false, indicating that we're applying a
-- scalar test
EXPLAIN (COSTS OFF)
SELECT id FROM dist_tab WHERE (val_null, val_null) IS NOT DISTINCT FROM NULL::RECORD;
SELECT id FROM dist_tab WHERE (val_null, val_null) IS NOT DISTINCT FROM NULL::RECORD;

DROP TABLE dist_tab;
DROP TYPE dist_row_t;

--
-- Test optimization of BooleanTest (IS [NOT] TRUE/FALSE/UNKNOWN) on
-- non-nullable input
--
CREATE TABLE bool_tab (id int, flag_nn boolean NOT NULL, flag_null boolean);

INSERT INTO bool_tab VALUES (1, true,  true);
INSERT INTO bool_tab VALUES (2, false, NULL);

CREATE INDEX bool_tab_nn_idx ON bool_tab (flag_nn);

ANALYZE bool_tab;

-- Ensure that the predicate folds to constant FALSE
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_nn IS UNKNOWN;
SELECT id FROM bool_tab WHERE flag_nn IS UNKNOWN;

-- Ensure that the predicate folds to constant TRUE
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_nn IS NOT UNKNOWN;
SELECT id FROM bool_tab WHERE flag_nn IS NOT UNKNOWN;

-- Ensure that the predicate folds to flag_nn
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_nn IS TRUE;
SELECT id FROM bool_tab WHERE flag_nn IS TRUE;

-- Ensure that the predicate folds to flag_nn, and thus can use index scan
SET enable_seqscan TO off;
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_nn IS NOT FALSE;
SELECT id FROM bool_tab WHERE flag_nn IS NOT FALSE;
RESET enable_seqscan;

-- Ensure that the predicate folds to not flag_nn
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_nn IS FALSE;
SELECT id FROM bool_tab WHERE flag_nn IS FALSE;

-- Ensure that the predicate folds to not flag_nn, and thus can use index scan
SET enable_seqscan TO off;
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_nn IS NOT TRUE;
SELECT id FROM bool_tab WHERE flag_nn IS NOT TRUE;
RESET enable_seqscan;

-- Ensure that the predicate is preserved as a BooleanTest
EXPLAIN (COSTS OFF)
SELECT id FROM bool_tab WHERE flag_null IS UNKNOWN;
SELECT id FROM bool_tab WHERE flag_null IS UNKNOWN;

DROP TABLE bool_tab;
