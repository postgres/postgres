--
-- POLYGON
--
-- polygon logic
--

CREATE TABLE POLYGON_TBL(f1 polygon);


INSERT INTO POLYGON_TBL(f1) VALUES ('(2.0,0.0),(2.0,4.0),(0.0,0.0)');

INSERT INTO POLYGON_TBL(f1) VALUES ('(3.0,1.0),(3.0,3.0),(1.0,0.0)');

-- degenerate polygons
INSERT INTO POLYGON_TBL(f1) VALUES ('(0.0,0.0)');

INSERT INTO POLYGON_TBL(f1) VALUES ('(0.0,1.0),(0.0,1.0)');

-- bad polygon input strings
INSERT INTO POLYGON_TBL(f1) VALUES ('0.0');

INSERT INTO POLYGON_TBL(f1) VALUES ('(0.0 0.0');

INSERT INTO POLYGON_TBL(f1) VALUES ('(0,1,2)');

INSERT INTO POLYGON_TBL(f1) VALUES ('(0,1,2,3');

INSERT INTO POLYGON_TBL(f1) VALUES ('asdf');


SELECT '' AS four, * FROM POLYGON_TBL;

-- overlap
SELECT '' AS three, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 && '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- left overlap
SELECT '' AS four, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 &< '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- right overlap
SELECT '' AS two, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 &> '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- left of
SELECT '' AS one, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 << '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- right of
SELECT '' AS zero, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 >> '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- contained
SELECT '' AS one, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 <@ polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- same
SELECT '' AS one, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 ~= polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

-- contains
SELECT '' AS one, p.*
   FROM POLYGON_TBL p
   WHERE p.f1 @> polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)';

--
-- polygon logic
--
-- left of
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' << polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

-- left overlap
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' << polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS true;

-- right overlap
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' &> polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

-- right of
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' >> polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

-- contained in
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' <@ polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

-- contains
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' @> polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

SELECT '((0,4),(6,4),(1,2),(6,0),(0,0))'::polygon @> '((2,1),(2,3),(3,3),(3,1))'::polygon AS "false";

SELECT '((0,4),(6,4),(3,2),(6,0),(0,0))'::polygon @> '((2,1),(2,3),(3,3),(3,1))'::polygon AS "true";

SELECT '((1,1),(1,4),(5,4),(5,3),(2,3),(2,2),(5,2),(5,1))'::polygon @> '((3,2),(3,3),(4,3),(4,2))'::polygon AS "false";

SELECT '((0,0),(0,3),(3,3),(3,0))'::polygon @> '((2,1),(2,2),(3,2),(3,1))'::polygon AS "true";

-- same
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' ~= polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

-- overlap
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' && polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS true;

SELECT '((0,4),(6,4),(1,2),(6,0),(0,0))'::polygon && '((2,1),(2,3),(3,3),(3,1))'::polygon AS "true";

SELECT '((1,4),(1,1),(4,1),(4,2),(2,2),(2,4),(1,4))'::polygon && '((3,3),(4,3),(4,4),(3,4),(3,3))'::polygon AS "false";
SELECT '((200,800),(800,800),(800,200),(200,200))' &&  '(1000,1000,0,0)'::polygon AS "true";

-- distance from a point
SELECT	'(0,0)'::point <-> '((0,0),(1,2),(2,1))'::polygon as on_corner,
	'(1,1)'::point <-> '((0,0),(2,2),(1,3))'::polygon as on_segment,
	'(2,2)'::point <-> '((0,0),(1,4),(3,1))'::polygon as inside,
	'(3,3)'::point <-> '((0,2),(2,0),(2,2))'::polygon as near_corner,
	'(4,4)'::point <-> '((0,0),(0,3),(4,0))'::polygon as near_segment;

--
-- Test the SP-GiST index
--

CREATE TABLE quad_poly_tbl (id int, p polygon);

INSERT INTO quad_poly_tbl
	SELECT (x - 1) * 100 + y, polygon(circle(point(x * 10, y * 10), 1 + (x + y) % 10))
	FROM generate_series(1, 100) x,
		 generate_series(1, 100) y;

INSERT INTO quad_poly_tbl
	SELECT i, polygon '((200, 300),(210, 310),(230, 290))'
	FROM generate_series(10001, 11000) AS i;

INSERT INTO quad_poly_tbl
	VALUES
		(11001, NULL),
		(11002, NULL),
		(11003, NULL);

CREATE INDEX quad_poly_tbl_idx ON quad_poly_tbl USING spgist(p);

-- get reference results for ORDER BY distance from seq scan
SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

CREATE TABLE quad_poly_tbl_ord_seq1 AS
SELECT rank() OVER (ORDER BY p <-> point '123,456') n, p <-> point '123,456' dist, id
FROM quad_poly_tbl;

CREATE TABLE quad_poly_tbl_ord_seq2 AS
SELECT rank() OVER (ORDER BY p <-> point '123,456') n, p <-> point '123,456' dist, id
FROM quad_poly_tbl WHERE p <@ polygon '((300,300),(400,600),(600,500),(700,200))';

-- check results results from index scan
SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p << polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p << polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p &< polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p &< polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p && polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p && polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p &> polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p &> polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p >> polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p >> polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p <<| polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p <<| polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p &<| polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p &<| polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p |&> polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p |&> polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p |>> polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p |>> polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p <@ polygon '((300,300),(400,600),(600,500),(700,200))';
SELECT count(*) FROM quad_poly_tbl WHERE p <@ polygon '((300,300),(400,600),(600,500),(700,200))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p @> polygon '((340,550),(343,552),(341,553))';
SELECT count(*) FROM quad_poly_tbl WHERE p @> polygon '((340,550),(343,552),(341,553))';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_poly_tbl WHERE p ~= polygon '((200, 300),(210, 310),(230, 290))';
SELECT count(*) FROM quad_poly_tbl WHERE p ~= polygon '((200, 300),(210, 310),(230, 290))';

-- test ORDER BY distance
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

EXPLAIN (COSTS OFF)
SELECT rank() OVER (ORDER BY p <-> point '123,456') n, p <-> point '123,456' dist, id
FROM quad_poly_tbl;

CREATE TEMP TABLE quad_poly_tbl_ord_idx1 AS
SELECT rank() OVER (ORDER BY p <-> point '123,456') n, p <-> point '123,456' dist, id
FROM quad_poly_tbl;

SELECT *
FROM quad_poly_tbl_ord_seq1 seq FULL JOIN quad_poly_tbl_ord_idx1 idx
	ON seq.n = idx.n AND seq.id = idx.id AND
		(seq.dist = idx.dist OR seq.dist IS NULL AND idx.dist IS NULL)
WHERE seq.id IS NULL OR idx.id IS NULL;


EXPLAIN (COSTS OFF)
SELECT rank() OVER (ORDER BY p <-> point '123,456') n, p <-> point '123,456' dist, id
FROM quad_poly_tbl WHERE p <@ polygon '((300,300),(400,600),(600,500),(700,200))';

CREATE TEMP TABLE quad_poly_tbl_ord_idx2 AS
SELECT rank() OVER (ORDER BY p <-> point '123,456') n, p <-> point '123,456' dist, id
FROM quad_poly_tbl WHERE p <@ polygon '((300,300),(400,600),(600,500),(700,200))';

SELECT *
FROM quad_poly_tbl_ord_seq2 seq FULL JOIN quad_poly_tbl_ord_idx2 idx
	ON seq.n = idx.n AND seq.id = idx.id AND
		(seq.dist = idx.dist OR seq.dist IS NULL AND idx.dist IS NULL)
WHERE seq.id IS NULL OR idx.id IS NULL;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
