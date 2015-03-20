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
