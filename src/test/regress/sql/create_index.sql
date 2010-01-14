--
-- CREATE_INDEX
-- Create ancillary data structures (i.e. indices)
--

--
-- BTREE
--
CREATE INDEX onek_unique1 ON onek USING btree(unique1 int4_ops);

CREATE INDEX onek_unique2 ON onek USING btree(unique2 int4_ops);

CREATE INDEX onek_hundred ON onek USING btree(hundred int4_ops);

CREATE INDEX onek_stringu1 ON onek USING btree(stringu1 name_ops);

CREATE INDEX tenk1_unique1 ON tenk1 USING btree(unique1 int4_ops);

CREATE INDEX tenk1_unique2 ON tenk1 USING btree(unique2 int4_ops);

CREATE INDEX tenk1_hundred ON tenk1 USING btree(hundred int4_ops);

CREATE INDEX tenk1_thous_tenthous ON tenk1 (thousand, tenthous);

CREATE INDEX tenk2_unique1 ON tenk2 USING btree(unique1 int4_ops);

CREATE INDEX tenk2_unique2 ON tenk2 USING btree(unique2 int4_ops);

CREATE INDEX tenk2_hundred ON tenk2 USING btree(hundred int4_ops);

CREATE INDEX rix ON road USING btree (name text_ops);

CREATE INDEX iix ON ihighway USING btree (name text_ops);

CREATE INDEX six ON shighway USING btree (name text_ops);

-- test comments
COMMENT ON INDEX six_wrong IS 'bad index';
COMMENT ON INDEX six IS 'good index';
COMMENT ON INDEX six IS NULL;

--
-- BTREE ascending/descending cases
--
-- we load int4/text from pure descending data (each key is a new
-- low key) and name/f8 from pure ascending data (each key is a new
-- high key).  we had a bug where new low keys would sometimes be
-- "lost".
--
CREATE INDEX bt_i4_index ON bt_i4_heap USING btree (seqno int4_ops);

CREATE INDEX bt_name_index ON bt_name_heap USING btree (seqno name_ops);

CREATE INDEX bt_txt_index ON bt_txt_heap USING btree (seqno text_ops);

CREATE INDEX bt_f8_index ON bt_f8_heap USING btree (seqno float8_ops);

--
-- BTREE partial indices
--
CREATE INDEX onek2_u1_prtl ON onek2 USING btree(unique1 int4_ops)
	where unique1 < 20 or unique1 > 980;

CREATE INDEX onek2_u2_prtl ON onek2 USING btree(unique2 int4_ops)
	where stringu1 < 'B';

CREATE INDEX onek2_stu1_prtl ON onek2 USING btree(stringu1 name_ops)
	where onek2.stringu1 >= 'J' and onek2.stringu1 < 'K';

--
-- GiST (rtree-equivalent opclasses only)
--
CREATE INDEX grect2ind ON fast_emp4000 USING gist (home_base);

CREATE INDEX gpolygonind ON polygon_tbl USING gist (f1);

CREATE INDEX gcircleind ON circle_tbl USING gist (f1);

CREATE INDEX gpointind ON point_tbl USING gist (f1);

CREATE TEMP TABLE gpolygon_tbl AS
    SELECT polygon(home_base) AS f1 FROM slow_emp4000;
INSERT INTO gpolygon_tbl VALUES ( '(1000,0,0,1000)' ); 
INSERT INTO gpolygon_tbl VALUES ( '(0,1000,1000,1000)' ); 

CREATE TEMP TABLE gcircle_tbl AS
    SELECT circle(home_base) AS f1 FROM slow_emp4000;

CREATE INDEX ggpolygonind ON gpolygon_tbl USING gist (f1);

CREATE INDEX ggcircleind ON gcircle_tbl USING gist (f1);

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

SELECT * FROM fast_emp4000
    WHERE home_base @ '(200,200),(2000,1000)'::box
    ORDER BY (home_base[0])[0];

SELECT count(*) FROM fast_emp4000 WHERE home_base && '(1000,1000,0,0)'::box;

SELECT count(*) FROM fast_emp4000 WHERE home_base IS NULL;

SELECT * FROM polygon_tbl WHERE f1 ~ '((1,1),(2,2),(2,1))'::polygon
    ORDER BY (poly_center(f1))[0];

SELECT * FROM circle_tbl WHERE f1 && circle(point(1,-2), 1)
    ORDER BY area(f1);

SELECT count(*) FROM gpolygon_tbl WHERE f1 && '(1000,1000,0,0)'::polygon;

SELECT count(*) FROM gcircle_tbl WHERE f1 && '<(500,500),500>'::circle;

SELECT count(*) FROM point_tbl WHERE f1 <@ box '(0,0,100,100)';

SELECT count(*) FROM point_tbl WHERE box '(0,0,100,100)' @> f1;

SELECT count(*) FROM point_tbl WHERE f1 <@ polygon '(0,0),(0,100),(100,100),(50,50),(100,0),(0,0)';

SELECT count(*) FROM point_tbl WHERE f1 <@ circle '<(50,50),50>';

SELECT count(*) FROM point_tbl p WHERE p.f1 << '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 >> '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 <^ '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 >^ '(0.0, 0.0)';

SELECT count(*) FROM point_tbl p WHERE p.f1 ~= '(-5, -12)';

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = ON;

EXPLAIN (COSTS OFF)
SELECT * FROM fast_emp4000
    WHERE home_base @ '(200,200),(2000,1000)'::box
    ORDER BY (home_base[0])[0];
SELECT * FROM fast_emp4000
    WHERE home_base @ '(200,200),(2000,1000)'::box
    ORDER BY (home_base[0])[0];

EXPLAIN (COSTS OFF)
SELECT count(*) FROM fast_emp4000 WHERE home_base && '(1000,1000,0,0)'::box;
SELECT count(*) FROM fast_emp4000 WHERE home_base && '(1000,1000,0,0)'::box;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM fast_emp4000 WHERE home_base IS NULL;
SELECT count(*) FROM fast_emp4000 WHERE home_base IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM polygon_tbl WHERE f1 ~ '((1,1),(2,2),(2,1))'::polygon
    ORDER BY (poly_center(f1))[0];
SELECT * FROM polygon_tbl WHERE f1 ~ '((1,1),(2,2),(2,1))'::polygon
    ORDER BY (poly_center(f1))[0];

EXPLAIN (COSTS OFF)
SELECT * FROM circle_tbl WHERE f1 && circle(point(1,-2), 1)
    ORDER BY area(f1);
SELECT * FROM circle_tbl WHERE f1 && circle(point(1,-2), 1)
    ORDER BY area(f1);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM gpolygon_tbl WHERE f1 && '(1000,1000,0,0)'::polygon;
SELECT count(*) FROM gpolygon_tbl WHERE f1 && '(1000,1000,0,0)'::polygon;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM gcircle_tbl WHERE f1 && '<(500,500),500>'::circle;
SELECT count(*) FROM gcircle_tbl WHERE f1 && '<(500,500),500>'::circle;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE f1 <@ box '(0,0,100,100)';
SELECT count(*) FROM point_tbl WHERE f1 <@ box '(0,0,100,100)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE box '(0,0,100,100)' @> f1;
SELECT count(*) FROM point_tbl WHERE box '(0,0,100,100)' @> f1;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE f1 <@ polygon '(0,0),(0,100),(100,100),(50,50),(100,0),(0,0)';
SELECT count(*) FROM point_tbl WHERE f1 <@ polygon '(0,0),(0,100),(100,100),(50,50),(100,0),(0,0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl WHERE f1 <@ circle '<(50,50),50>';
SELECT count(*) FROM point_tbl WHERE f1 <@ circle '<(50,50),50>';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 << '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 << '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 >> '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 >> '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 <^ '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 <^ '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 >^ '(0.0, 0.0)';
SELECT count(*) FROM point_tbl p WHERE p.f1 >^ '(0.0, 0.0)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM point_tbl p WHERE p.f1 ~= '(-5, -12)';
SELECT count(*) FROM point_tbl p WHERE p.f1 ~= '(-5, -12)';

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--
-- GIN over int[] and text[]
--

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

CREATE INDEX intarrayidx ON array_index_op_test USING gin (i);

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{38,34,32,89}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i = '{47,77}' ORDER BY seqno;

CREATE INDEX textarrayidx ON array_index_op_test USING gin (t);

SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAA72908}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t <@ '{AAAAAAAA72908,AAAAAAAAAAAAAAAAAAA17075,AA88409,AAAAAAAAAAAAAAAAAA36842,AAAAAAA48038,AAAAAAAAAAAAAA10611}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{AAAAAAAAAA646,A87088}' ORDER BY seqno;

-- Repeat some of the above tests but exercising bitmapscans instead
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{38,34,32,89}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i = '{47,77}' ORDER BY seqno;

-- And try it with a multicolumn GIN index

DROP INDEX intarrayidx, textarrayidx;

CREATE INDEX botharrayidx ON array_index_op_test USING gin (i, t);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32}' AND t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' AND t @> '{AAAAAAA80240}' ORDER BY seqno;

SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32}' AND t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' AND t @> '{AAAAAAA80240}' ORDER BY seqno;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--
-- HASH
--
CREATE INDEX hash_i4_index ON hash_i4_heap USING hash (random int4_ops);

CREATE INDEX hash_name_index ON hash_name_heap USING hash (random name_ops);

CREATE INDEX hash_txt_index ON hash_txt_heap USING hash (random text_ops);

CREATE INDEX hash_f8_index ON hash_f8_heap USING hash (random float8_ops);

-- CREATE INDEX hash_ovfl_index ON hash_ovfl_heap USING hash (x int4_ops);


--
-- Test functional index
--
CREATE TABLE func_index_heap (f1 text, f2 text);
CREATE UNIQUE INDEX func_index_index on func_index_heap (textcat(f1,f2));

INSERT INTO func_index_heap VALUES('ABC','DEF');
INSERT INTO func_index_heap VALUES('AB','CDEFG');
INSERT INTO func_index_heap VALUES('QWE','RTY');
-- this should fail because of unique index:
INSERT INTO func_index_heap VALUES('ABCD', 'EF');
-- but this shouldn't:
INSERT INTO func_index_heap VALUES('QWERTY');


--
-- Same test, expressional index
--
DROP TABLE func_index_heap;
CREATE TABLE func_index_heap (f1 text, f2 text);
CREATE UNIQUE INDEX func_index_index on func_index_heap ((f1 || f2) text_ops);

INSERT INTO func_index_heap VALUES('ABC','DEF');
INSERT INTO func_index_heap VALUES('AB','CDEFG');
INSERT INTO func_index_heap VALUES('QWE','RTY');
-- this should fail because of unique index:
INSERT INTO func_index_heap VALUES('ABCD', 'EF');
-- but this shouldn't:
INSERT INTO func_index_heap VALUES('QWERTY');

--
-- Also try building functional, expressional, and partial indexes on
-- tables that already contain data.
--
create unique index hash_f8_index_1 on hash_f8_heap(abs(random));
create unique index hash_f8_index_2 on hash_f8_heap((seqno + 1), random);
create unique index hash_f8_index_3 on hash_f8_heap(random) where seqno > 1000;

--
-- Try some concurrent index builds
--
-- Unfortunately this only tests about half the code paths because there are
-- no concurrent updates happening to the table at the same time.

CREATE TABLE concur_heap (f1 text, f2 text);
-- empty table
CREATE INDEX CONCURRENTLY concur_index1 ON concur_heap(f2,f1);
INSERT INTO concur_heap VALUES  ('a','b');
INSERT INTO concur_heap VALUES  ('b','b');
-- unique index
CREATE UNIQUE INDEX CONCURRENTLY concur_index2 ON concur_heap(f1);
-- check if constraint is set up properly to be enforced
INSERT INTO concur_heap VALUES ('b','x');
-- check if constraint is enforced properly at build time
CREATE UNIQUE INDEX CONCURRENTLY concur_index3 ON concur_heap(f2);
-- test that expression indexes and partial indexes work concurrently
CREATE INDEX CONCURRENTLY concur_index4 on concur_heap(f2) WHERE f1='a';
CREATE INDEX CONCURRENTLY concur_index5 on concur_heap(f2) WHERE f1='x';
-- here we also check that you can default the index name
CREATE INDEX CONCURRENTLY on concur_heap((f2||f1));

-- You can't do a concurrent index build in a transaction
BEGIN;
CREATE INDEX CONCURRENTLY concur_index7 ON concur_heap(f1);
COMMIT;

-- But you can do a regular index build in a transaction
BEGIN;
CREATE INDEX std_index on concur_heap(f2);
COMMIT;

-- check to make sure that the failed indexes were cleaned up properly and the
-- successful indexes are created properly. Notably that they do NOT have the
-- "invalid" flag set.

\d concur_heap

DROP TABLE concur_heap;

--
-- Tests for IS NULL/IS NOT NULL with b-tree indexes
--

SELECT unique1, unique2 INTO onek_with_null FROM onek;
INSERT INTO onek_with_null (unique1,unique2) VALUES (NULL, -1), (NULL, NULL);
CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2,unique1);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = ON;

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2 desc,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2 desc nulls last,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2  nulls first,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
 
DROP TABLE onek_with_null;
