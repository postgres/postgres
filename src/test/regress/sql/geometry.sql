--
-- Points
--

SELECT '' AS xxx, center(f1) AS center
   FROM BOX_TBL;

SELECT '' AS xxx, (@@ f1) AS center
   FROM BOX_TBL;

SELECT '' AS xxx, point(f1) AS center
   FROM CIRCLE_TBL;

SELECT '' AS xxx, (@@ f1) AS center
   FROM CIRCLE_TBL;

SELECT '' AS xxx, (@@ f1) AS center
   FROM POLYGON_TBL
   WHERE (# f1) > 2;

-- "is horizontal" function
SELECT '' AS two, p1.f1
   FROM POINT_TBL p1
   WHERE ishorizontal(p1.f1, '(0,0)'::point);

-- "is horizontal" operator
SELECT '' AS two, p1.f1
   FROM POINT_TBL p1
   WHERE p1.f1 ?- '(0,0)'::point;

-- "is vertical" function
SELECT '' AS one, p1.f1
   FROM POINT_TBL p1
   WHERE isvertical(p1.f1, '(5.1,34.5)'::point);

-- "is vertical" operator
SELECT '' AS one, p1.f1
   FROM POINT_TBL p1
   WHERE p1.f1 ?| '(5.1,34.5)'::point;

--
-- Line segments
--

-- intersection
SELECT '' AS xxx, p.f1, l.s, l.s # p.f1 AS intersection
   FROM LSEG_TBL l, POINT_TBL p;

-- closest point
SELECT '' AS xxx, p.f1, l.s, p.f1 ## l.s AS closest
   FROM LSEG_TBL l, POINT_TBL p;

--
-- Lines
--

--
-- Boxes
--

SELECT box(f1) AS box FROM CIRCLE_TBL;

-- translation
SELECT '' AS count, b.f1 + p.f1 AS translation
   FROM BOX_TBL b, POINT_TBL p;

SELECT '' AS count, b.f1 - p.f1 AS translation
   FROM BOX_TBL b, POINT_TBL p;

-- scaling and rotation
SELECT '' AS count, b.f1 * p.f1 AS rotation
   FROM BOX_TBL b, POINT_TBL p;

SELECT '' AS count, b.f1 / p.f1 AS rotation
   FROM BOX_TBL b, POINT_TBL p
   WHERE (p.f1 <-> '(0,0)'::point) >= 1;

--
-- Paths
--

SET geqo TO 'off';

SELECT '' AS xxx, points(f1) AS npoints, f1 AS path FROM PATH_TBL;

SELECT '' AS xxx, path(f1) FROM POLYGON_TBL;

-- translation
SELECT '' AS eight, p1.f1 + '(10,10)'::point AS dist_add
   FROM PATH_TBL p1;

-- scaling and rotation
SELECT '' AS eight, p1.f1 * '(2,-1)'::point AS dist_mul
   FROM PATH_TBL p1;

RESET geqo;

--
-- Polygons
--

-- containment
SELECT '' AS xxx, p.f1, poly.f1, poly.f1 ~ p.f1 AS contains
   FROM POLYGON_TBL poly, POINT_TBL p;

SELECT '' AS xxx, p.f1, poly.f1, p.f1 @ poly.f1 AS contained
   FROM POLYGON_TBL poly, POINT_TBL p;

SELECT '' AS xxx, points(f1) AS npoints, f1 AS polygon
   FROM POLYGON_TBL;

SELECT '' AS xxx, polygon(f1)
   FROM BOX_TBL;

SELECT '' AS xxx, polygon(f1)
   FROM PATH_TBL WHERE isclosed(f1);

SELECT '' AS xxx, f1 AS open_path, polygon( pclose(f1)) AS polygon
   FROM PATH_TBL
   WHERE isopen(f1);

-- convert circles to polygons using the default number of points
SELECT '' AS xxx, polygon(f1)
   FROM CIRCLE_TBL;

-- convert the circle to an 8-point polygon
SELECT '' AS xxx, polygon(8, f1)
   FROM CIRCLE_TBL;

--
-- Circles
--

SELECT '' AS xxx, circle(f1, 50.0)
   FROM POINT_TBL;

SELECT '' AS xxx, circle(f1)
   FROM BOX_TBL;

SELECT '' AS xxx, circle(f1)
   FROM POLYGON_TBL
   WHERE (# f1) >= 2;

SELECT '' AS twentyfour, c1.f1 AS circle, p1.f1 AS point, (p1.f1 <-> c1.f1) AS distance
   FROM CIRCLE_TBL c1, POINT_TBL p1
   WHERE (p1.f1 <-> c1.f1) > 0
   ORDER BY distance, circle;

