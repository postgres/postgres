--
-- GEOMETRY
--

-- Back off displayed precision a little bit to reduce platform-to-platform
-- variation in results.
SET extra_float_digits TO -3;

--
-- Points
--

SELECT '' AS four, center(f1) AS center
   FROM BOX_TBL;

SELECT '' AS four, (@@ f1) AS center
   FROM BOX_TBL;

SELECT '' AS six, point(f1) AS center
   FROM CIRCLE_TBL;

SELECT '' AS six, (@@ f1) AS center
   FROM CIRCLE_TBL;

SELECT '' AS two, (@@ f1) AS center
   FROM POLYGON_TBL
   WHERE (# f1) > 2;

-- "is horizontal" function
SELECT '' AS two, p1.f1
   FROM POINT_TBL p1
   WHERE ishorizontal(p1.f1, point '(0,0)');

-- "is horizontal" operator
SELECT '' AS two, p1.f1
   FROM POINT_TBL p1
   WHERE p1.f1 ?- point '(0,0)';

-- "is vertical" function
SELECT '' AS one, p1.f1
   FROM POINT_TBL p1
   WHERE isvertical(p1.f1, point '(5.1,34.5)');

-- "is vertical" operator
SELECT '' AS one, p1.f1
   FROM POINT_TBL p1
   WHERE p1.f1 ?| point '(5.1,34.5)';

--
-- Line segments
--

-- intersection
SELECT '' AS count, p.f1, l.s, l.s # p.f1 AS intersection
   FROM LSEG_TBL l, POINT_TBL p;

-- closest point
SELECT '' AS thirty, p.f1, l.s, p.f1 ## l.s AS closest
   FROM LSEG_TBL l, POINT_TBL p;

--
-- Boxes
--

SELECT '' as six, box(f1) AS box FROM CIRCLE_TBL;

-- translation
SELECT '' AS twentyfour, b.f1 + p.f1 AS translation
   FROM BOX_TBL b, POINT_TBL p;

SELECT '' AS twentyfour, b.f1 - p.f1 AS translation
   FROM BOX_TBL b, POINT_TBL p;

-- scaling and rotation
SELECT '' AS twentyfour, b.f1 * p.f1 AS rotation
   FROM BOX_TBL b, POINT_TBL p;

SELECT '' AS twenty, b.f1 / p.f1 AS rotation
   FROM BOX_TBL b, POINT_TBL p
   WHERE (p.f1 <-> point '(0,0)') >= 1;

SELECT f1::box
	FROM POINT_TBL;

SELECT bound_box(a.f1, b.f1)
	FROM BOX_TBL a, BOX_TBL b;

--
-- Paths
--

SELECT '' AS eight, npoints(f1) AS npoints, f1 AS path FROM PATH_TBL;

SELECT '' AS four, path(f1) FROM POLYGON_TBL;

-- translation
SELECT '' AS eight, p1.f1 + point '(10,10)' AS dist_add
   FROM PATH_TBL p1;

-- scaling and rotation
SELECT '' AS eight, p1.f1 * point '(2,-1)' AS dist_mul
   FROM PATH_TBL p1;

--
-- Polygons
--

-- containment
SELECT '' AS twentyfour, p.f1, poly.f1, poly.f1 @> p.f1 AS contains
   FROM POLYGON_TBL poly, POINT_TBL p;

SELECT '' AS twentyfour, p.f1, poly.f1, p.f1 <@ poly.f1 AS contained
   FROM POLYGON_TBL poly, POINT_TBL p;

SELECT '' AS four, npoints(f1) AS npoints, f1 AS polygon
   FROM POLYGON_TBL;

SELECT '' AS four, polygon(f1)
   FROM BOX_TBL;

SELECT '' AS four, polygon(f1)
   FROM PATH_TBL WHERE isclosed(f1);

SELECT '' AS four, f1 AS open_path, polygon( pclose(f1)) AS polygon
   FROM PATH_TBL
   WHERE isopen(f1);

-- convert circles to polygons using the default number of points
SELECT '' AS six, polygon(f1)
   FROM CIRCLE_TBL;

-- convert the circle to an 8-point polygon
SELECT '' AS six, polygon(8, f1)
   FROM CIRCLE_TBL;

--
-- Circles
--

SELECT '' AS six, circle(f1, 50.0)
   FROM POINT_TBL;

SELECT '' AS four, circle(f1)
   FROM BOX_TBL;

SELECT '' AS two, circle(f1)
   FROM POLYGON_TBL
   WHERE (# f1) >= 3;

SELECT '' AS twentyfour, c1.f1 AS circle, p1.f1 AS point, (p1.f1 <-> c1.f1) AS distance
   FROM CIRCLE_TBL c1, POINT_TBL p1
   WHERE (p1.f1 <-> c1.f1) > 0
   ORDER BY distance, area(c1.f1), p1.f1[0];
