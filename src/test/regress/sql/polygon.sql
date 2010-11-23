--
-- POLYGON
--
-- polygon logic
--
-- 3	      o
--	      |
-- 2	    + |
--	   /  |
-- 1	  # o +
--       /    |
-- 0	#-----o-+
--
--	0 1 2 3 4
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
-- 3	      o
--	      |
-- 2	    + |
--	   /  |
-- 1	  / o +
--       /    |
-- 0	+-----o-+
--
--	0 1 2 3 4
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

--     +------------------------+
--     |    *---*               1
--     |  + |   |
--     |  2 *---*
--     +------------------------+
--                              3
--     endpoints '+' is ofr one polygon, '*' - for another
--     Edges 1-2, 2-3 are not shown on picture
SELECT '((0,4),(6,4),(1,2),(6,0),(0,0))'::polygon @> '((2,1),(2,3),(3,3),(3,1))'::polygon AS "false";

--     +-----------+
--     |    *---* /
--     |    |   |/
--     |    |   +
--     |    |   |\
--     |    *---* \
--     +-----------+
SELECT '((0,4),(6,4),(3,2),(6,0),(0,0))'::polygon @> '((2,1),(2,3),(3,3),(3,1))'::polygon AS "true";

--     +-----------------+
--     |                 |
--     |   +---*---*-----+
--     |   |   |   |
--     |   +---*---*-----+
--     |                 |
--     +-----------------+
SELECT '((1,1),(1,4),(5,4),(5,3),(2,3),(2,2),(5,2),(5,1))'::polygon @> '((3,2),(3,3),(4,3),(4,2))'::polygon AS "false";

--     +---------+
--     |         |
--     |    *----*
--     |    |    |
--     |    *----*
--     |         |
--     +---------+
SELECT '((0,0),(0,3),(3,3),(3,0))'::polygon @> '((2,1),(2,2),(3,2),(3,1))'::polygon AS "true";

-- same
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' ~= polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS false;

-- overlap
SELECT polygon '(2.0,0.0),(2.0,4.0),(0.0,0.0)' && polygon '(3.0,1.0),(3.0,3.0),(1.0,0.0)' AS true;

--     +--------------------+
--     |    *---*       	1
--     |  + |   |
--     |  2 *---*
--     +--------------------+
--                      	3
--     Edges 1-2, 2-3 are not shown on picture
SELECT '((0,4),(6,4),(1,2),(6,0),(0,0))'::polygon && '((2,1),(2,3),(3,3),(3,1))'::polygon AS "true";

--     +--+ *--*
--     |  | |  |
--     |  | *--*
--     |  +----+
--     |       |
--     +-------+
SELECT '((1,4),(1,1),(4,1),(4,2),(2,2),(2,4),(1,4))'::polygon && '((3,3),(4,3),(4,4),(3,4),(3,3))'::polygon AS "false";
SELECT '((200,800),(800,800),(800,200),(200,200))' &&  '(1000,1000,0,0)'::polygon AS "true";
