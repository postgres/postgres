--
-- BOX
--

--
-- box logic
--	     o
-- 3	  o--|X
--	  |  o|
-- 2	+-+-+ |
--	| | | |
-- 1	| o-+-o
--	|   |
-- 0	+---+
--
--	0 1 2 3
--

-- boxes are specified by two points, given by four floats x1,y1,x2,y2


CREATE TABLE BOX_TBL (f1 box);

INSERT INTO BOX_TBL (f1) VALUES ('(2.0,2.0,0.0,0.0)');

INSERT INTO BOX_TBL (f1) VALUES ('(1.0,1.0,3.0,3.0)');

INSERT INTO BOX_TBL (f1) VALUES ('((-8, 2), (-2, -10))');


-- degenerate cases where the box is a line or a point
-- note that lines and points boxes all have zero area
INSERT INTO BOX_TBL (f1) VALUES ('(2.5, 2.5, 2.5,3.5)');

INSERT INTO BOX_TBL (f1) VALUES ('(3.0, 3.0,3.0,3.0)');

-- badly formatted box inputs
INSERT INTO BOX_TBL (f1) VALUES ('(2.3, 4.5)');

INSERT INTO BOX_TBL (f1) VALUES ('[1, 2, 3, 4)');

INSERT INTO BOX_TBL (f1) VALUES ('(1, 2, 3, 4]');

INSERT INTO BOX_TBL (f1) VALUES ('(1, 2, 3, 4) x');

INSERT INTO BOX_TBL (f1) VALUES ('asdfasdf(ad');


SELECT * FROM BOX_TBL;

SELECT b.*, area(b.f1) as barea
   FROM BOX_TBL b;

-- overlap
SELECT b.f1
   FROM BOX_TBL b
   WHERE b.f1 && box '(2.5,2.5,1.0,1.0)';

-- left-or-overlap (x only)
SELECT b1.*
   FROM BOX_TBL b1
   WHERE b1.f1 &< box '(2.0,2.0,2.5,2.5)';

-- right-or-overlap (x only)
SELECT b1.*
   FROM BOX_TBL b1
   WHERE b1.f1 &> box '(2.0,2.0,2.5,2.5)';

-- left of
SELECT b.f1
   FROM BOX_TBL b
   WHERE b.f1 << box '(3.0,3.0,5.0,5.0)';

-- area <=
SELECT b.f1
   FROM BOX_TBL b
   WHERE b.f1 <= box '(3.0,3.0,5.0,5.0)';

-- area <
SELECT b.f1
   FROM BOX_TBL b
   WHERE b.f1 < box '(3.0,3.0,5.0,5.0)';

-- area =
SELECT b.f1
   FROM BOX_TBL b
   WHERE b.f1 = box '(3.0,3.0,5.0,5.0)';

-- area >
SELECT b.f1
   FROM BOX_TBL b				-- zero area
   WHERE b.f1 > box '(3.5,3.0,4.5,3.0)';

-- area >=
SELECT b.f1
   FROM BOX_TBL b				-- zero area
   WHERE b.f1 >= box '(3.5,3.0,4.5,3.0)';

-- right of
SELECT b.f1
   FROM BOX_TBL b
   WHERE box '(3.0,3.0,5.0,5.0)' >> b.f1;

-- contained in
SELECT b.f1
   FROM BOX_TBL b
   WHERE b.f1 <@ box '(0,0,3,3)';

-- contains
SELECT b.f1
   FROM BOX_TBL b
   WHERE box '(0,0,3,3)' @> b.f1;

-- box equality
SELECT b.f1
   FROM BOX_TBL b
   WHERE box '(1,1,3,3)' ~= b.f1;

-- center of box, left unary operator
SELECT @@(b1.f1) AS p
   FROM BOX_TBL b1;

-- wholly-contained
SELECT b1.*, b2.*
   FROM BOX_TBL b1, BOX_TBL b2
   WHERE b1.f1 @> b2.f1 and not b1.f1 ~= b2.f1;

SELECT height(f1), width(f1) FROM BOX_TBL;

--
-- Test the SP-GiST index
--

CREATE TEMPORARY TABLE box_temp (f1 box);

INSERT INTO box_temp
	SELECT box(point(i, i), point(i * 2, i * 2))
	FROM generate_series(1, 50) AS i;

CREATE INDEX box_spgist ON box_temp USING spgist (f1);

INSERT INTO box_temp
	VALUES (NULL),
		   ('(0,0)(0,100)'),
		   ('(-3,4.3333333333)(40,1)'),
		   ('(0,100)(0,infinity)'),
		   ('(-infinity,0)(0,infinity)'),
		   ('(-infinity,-infinity)(infinity,infinity)');

SET enable_seqscan = false;

SELECT * FROM box_temp WHERE f1 << '(10,20),(30,40)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 << '(10,20),(30,40)';

SELECT * FROM box_temp WHERE f1 &< '(10,4.333334),(5,100)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 &< '(10,4.333334),(5,100)';

SELECT * FROM box_temp WHERE f1 && '(15,20),(25,30)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 && '(15,20),(25,30)';

SELECT * FROM box_temp WHERE f1 &> '(40,30),(45,50)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 &> '(40,30),(45,50)';

SELECT * FROM box_temp WHERE f1 >> '(30,40),(40,30)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 >> '(30,40),(40,30)';

SELECT * FROM box_temp WHERE f1 <<| '(10,4.33334),(5,100)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 <<| '(10,4.33334),(5,100)';

SELECT * FROM box_temp WHERE f1 &<| '(10,4.3333334),(5,1)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 &<| '(10,4.3333334),(5,1)';

SELECT * FROM box_temp WHERE f1 |&> '(49.99,49.99),(49.99,49.99)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 |&> '(49.99,49.99),(49.99,49.99)';

SELECT * FROM box_temp WHERE f1 |>> '(37,38),(39,40)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 |>> '(37,38),(39,40)';

SELECT * FROM box_temp WHERE f1 @> '(10,11),(15,16)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 @> '(10,11),(15,15)';

SELECT * FROM box_temp WHERE f1 <@ '(10,15),(30,35)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 <@ '(10,15),(30,35)';

SELECT * FROM box_temp WHERE f1 ~= '(20,20),(40,40)';
EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 ~= '(20,20),(40,40)';

RESET enable_seqscan;

DROP INDEX box_spgist;

--
-- Test the SP-GiST index on the larger volume of data
--
CREATE TABLE quad_box_tbl (id int, b box);

INSERT INTO quad_box_tbl
  SELECT (x - 1) * 100 + y, box(point(x * 10, y * 10), point(x * 10 + 5, y * 10 + 5))
  FROM generate_series(1, 100) x,
       generate_series(1, 100) y;

-- insert repeating data to test allTheSame
INSERT INTO quad_box_tbl
  SELECT i, '((200, 300),(210, 310))'
  FROM generate_series(10001, 11000) AS i;

INSERT INTO quad_box_tbl
VALUES
  (11001, NULL),
  (11002, NULL),
  (11003, '((-infinity,-infinity),(infinity,infinity))'),
  (11004, '((-infinity,100),(-infinity,500))'),
  (11005, '((-infinity,-infinity),(700,infinity))');

CREATE INDEX quad_box_tbl_idx ON quad_box_tbl USING spgist(b);

-- get reference results for ORDER BY distance from seq scan
SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

CREATE TABLE quad_box_tbl_ord_seq1 AS
SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
FROM quad_box_tbl;

CREATE TABLE quad_box_tbl_ord_seq2 AS
SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
FROM quad_box_tbl WHERE b <@ box '((200,300),(500,600))';

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = ON;

SELECT count(*) FROM quad_box_tbl WHERE b <<  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b &<  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b &&  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b &>  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b >>  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b >>  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b <<| box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b &<| box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b |&> box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b |>> box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b @>  box '((201,301),(202,303))';
SELECT count(*) FROM quad_box_tbl WHERE b <@  box '((100,200),(300,500))';
SELECT count(*) FROM quad_box_tbl WHERE b ~=  box '((200,300),(205,305))';

-- test ORDER BY distance
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

EXPLAIN (COSTS OFF)
SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
FROM quad_box_tbl;

CREATE TEMP TABLE quad_box_tbl_ord_idx1 AS
SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
FROM quad_box_tbl;

SELECT *
FROM quad_box_tbl_ord_seq1 seq FULL JOIN quad_box_tbl_ord_idx1 idx
	ON seq.n = idx.n AND seq.id = idx.id AND
		(seq.dist = idx.dist OR seq.dist IS NULL AND idx.dist IS NULL)
WHERE seq.id IS NULL OR idx.id IS NULL;


EXPLAIN (COSTS OFF)
SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
FROM quad_box_tbl WHERE b <@ box '((200,300),(500,600))';

CREATE TEMP TABLE quad_box_tbl_ord_idx2 AS
SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
FROM quad_box_tbl WHERE b <@ box '((200,300),(500,600))';

SELECT *
FROM quad_box_tbl_ord_seq2 seq FULL JOIN quad_box_tbl_ord_idx2 idx
	ON seq.n = idx.n AND seq.id = idx.id AND
		(seq.dist = idx.dist OR seq.dist IS NULL AND idx.dist IS NULL)
WHERE seq.id IS NULL OR idx.id IS NULL;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- test non-error-throwing API for some core types
SELECT pg_input_is_valid('200', 'box');
SELECT * FROM pg_input_error_info('200', 'box');
SELECT pg_input_is_valid('((200,300),(500, xyz))', 'box');
SELECT * FROM pg_input_error_info('((200,300),(500, xyz))', 'box');
