--
-- Points
--

SELECT point(f1) FROM CIRCLE_TBL;

SELECT '' AS two, p1.f1
    FROM POINT_TBL p1
    WHERE ishorizontal(p1.f1, '(0,0)'::point);

SELECT '' AS one, p1.f1
    FROM POINT_TBL p1
    WHERE isvertical(p1.f1, '(5.1,34.5)'::point);

--
-- Line segments
--

--
-- Lines
--

--
-- Boxes
--

SELECT center(f1) FROM BOX_TBL;

SELECT box(f1) FROM CIRCLE_TBL;

-- translation
SELECT '' AS count, b.f1 + p.f1
    FROM BOX_TBL b, POINT_TBL p;

-- scaling and rotation
SELECT '' AS count, b.f1 * p.f1
    FROM BOX_TBL b, POINT_TBL p;

--
-- Paths
--

SET geqo TO 'off';

SELECT points(f1) AS npoints, f1 AS path FROM PATH_TBL;

SELECT path(f1) FROM POLYGON_TBL;

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

SELECT points(f1) AS npoints, f1 AS polygon FROM POLYGON_TBL;

SELECT polygon(f1) FROM BOX_TBL;

SELECT polygon(f1) FROM PATH_TBL WHERE isclosed(f1);

SELECT f1 AS open_path, polygon( pclose(f1)) AS polygon FROM PATH_TBL WHERE isopen(f1);

-- convert circles to polygons using the default number of points
SELECT polygon(f1) FROM CIRCLE_TBL;

-- convert the circle to an 8-point polygon
SELECT polygon(8, f1) FROM CIRCLE_TBL;

--
-- Circles
--

SELECT circle( f1, 50.0) FROM POINT_TBL;

SELECT '' AS twentyfour, c1.f1 AS circle, p1.f1 AS point, (p1.f1 <===> c1.f1) AS distance
 from CIRCLE_TBL c1, POINT_TBL p1
 WHERE (p1.f1 <===> c1.f1) > 0
 ORDER BY distance, circle;

