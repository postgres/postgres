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

-- Slope
SELECT p1.f1, p2.f1, slope(p1.f1, p2.f1) FROM POINT_TBL p1, POINT_TBL p2;

-- Add point
SELECT p1.f1, p2.f1, p1.f1 + p2.f1 FROM POINT_TBL p1, POINT_TBL p2;

-- Subtract point
SELECT p1.f1, p2.f1, p1.f1 - p2.f1 FROM POINT_TBL p1, POINT_TBL p2;

-- Multiply with point
SELECT p1.f1, p2.f1, p1.f1 * p2.f1 FROM POINT_TBL p1, POINT_TBL p2 WHERE p1.f1[0] BETWEEN 1 AND 1000;

-- Underflow error
SELECT p1.f1, p2.f1, p1.f1 * p2.f1 FROM POINT_TBL p1, POINT_TBL p2 WHERE p1.f1[0] < 1;

-- Divide by point
SELECT p1.f1, p2.f1, p1.f1 / p2.f1 FROM POINT_TBL p1, POINT_TBL p2 WHERE p2.f1[0] BETWEEN 1 AND 1000;

-- Overflow error
SELECT p1.f1, p2.f1, p1.f1 / p2.f1 FROM POINT_TBL p1, POINT_TBL p2 WHERE p2.f1[0] > 1000;

-- Division by 0 error
SELECT p1.f1, p2.f1, p1.f1 / p2.f1 FROM POINT_TBL p1, POINT_TBL p2 WHERE p2.f1 ~= '(0,0)'::point;

-- Distance to line
SELECT p.f1, l.s, p.f1 <-> l.s AS dist_pl, l.s <-> p.f1 AS dist_lp FROM POINT_TBL p, LINE_TBL l;

-- Distance to line segment
SELECT p.f1, l.s, p.f1 <-> l.s AS dist_ps, l.s <-> p.f1 AS dist_sp FROM POINT_TBL p, LSEG_TBL l;

-- Distance to box
SELECT p.f1, b.f1, p.f1 <-> b.f1 AS dist_pb, b.f1 <-> p.f1 AS dist_bp FROM POINT_TBL p, BOX_TBL b;

-- Distance to path
SELECT p.f1, p1.f1, p.f1 <-> p1.f1 AS dist_ppath, p1.f1 <-> p.f1 AS dist_pathp FROM POINT_TBL p, PATH_TBL p1;

-- Distance to polygon
SELECT p.f1, p1.f1, p.f1 <-> p1.f1 AS dist_ppoly, p1.f1 <-> p.f1 AS dist_polyp FROM POINT_TBL p, POLYGON_TBL p1;

-- Closest point to line
SELECT p.f1, l.s, p.f1 ## l.s FROM POINT_TBL p, LINE_TBL l;

-- Closest point to line segment
SELECT p.f1, l.s, p.f1 ## l.s FROM POINT_TBL p, LSEG_TBL l;

-- Closest point to box
SELECT p.f1, b.f1, p.f1 ## b.f1 FROM POINT_TBL p, BOX_TBL b;

-- On line
SELECT p.f1, l.s FROM POINT_TBL p, LINE_TBL l WHERE p.f1 <@ l.s;

-- On line segment
SELECT p.f1, l.s FROM POINT_TBL p, LSEG_TBL l WHERE p.f1 <@ l.s;

-- On path
SELECT p.f1, p1.f1 FROM POINT_TBL p, PATH_TBL p1 WHERE p.f1 <@ p1.f1;

--
-- Lines
--

-- Vertical
SELECT s FROM LINE_TBL WHERE ?| s;

-- Horizontal
SELECT s FROM LINE_TBL WHERE ?- s;

-- Same as line
SELECT l1.s, l2.s FROM LINE_TBL l1, LINE_TBL l2 WHERE l1.s = l2.s;

-- Parallel to line
SELECT l1.s, l2.s FROM LINE_TBL l1, LINE_TBL l2 WHERE l1.s ?|| l2.s;

-- Perpendicular to line
SELECT l1.s, l2.s FROM LINE_TBL l1, LINE_TBL l2 WHERE l1.s ?-| l2.s;

-- Distance to line
SELECT l1.s, l2.s, l1.s <-> l2.s FROM LINE_TBL l1, LINE_TBL l2;

-- Distance to box
SELECT l.s, b.f1, l.s <-> b.f1 FROM LINE_TBL l, BOX_TBL b;
SELECT l.s, b.f1, b.f1 <-> l.s FROM LINE_TBL l, BOX_TBL b;

-- Intersect with line
SELECT l1.s, l2.s FROM LINE_TBL l1, LINE_TBL l2 WHERE l1.s ?# l2.s;

-- Intersect with box
SELECT l.s, b.f1 FROM LINE_TBL l, BOX_TBL b WHERE l.s ?# b.f1;

-- Intersection point with line
SELECT l1.s, l2.s, l1.s # l2.s FROM LINE_TBL l1, LINE_TBL l2;

-- Closest point to line segment
SELECT l.s, l1.s, l.s ## l1.s FROM LINE_TBL l, LSEG_TBL l1;

-- Closest point to box
SELECT l.s, b.f1, l.s ## b.f1 FROM LINE_TBL l, BOX_TBL b;

--
-- Line segments
--

-- intersection
SELECT '' AS count, p.f1, l.s, l.s # p.f1 AS intersection
   FROM LSEG_TBL l, POINT_TBL p;

-- Length
SELECT s, @-@ s FROM LSEG_TBL;

-- Vertical
SELECT s FROM LSEG_TBL WHERE ?| s;

-- Horizontal
SELECT s FROM LSEG_TBL WHERE ?- s;

-- Center
SELECT s, @@ s FROM LSEG_TBL;

-- To point
SELECT s, s::point FROM LSEG_TBL;

-- Has points less than line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s < l2.s;

-- Has points less than or equal to line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s <= l2.s;

-- Has points equal to line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s = l2.s;

-- Has points greater than or equal to line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s >= l2.s;

-- Has points greater than line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s > l2.s;

-- Has points not equal to line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s != l2.s;

-- Parallel with line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s ?|| l2.s;

-- Perpendicular with line segment
SELECT l1.s, l2.s FROM LSEG_TBL l1, LSEG_TBL l2 WHERE l1.s ?-| l2.s;

-- Distance to line
SELECT l.s, l1.s, l.s <-> l1.s AS dist_sl, l1.s <-> l.s AS dist_ls FROM LSEG_TBL l, LINE_TBL l1;

-- Distance to line segment
SELECT l1.s, l2.s, l1.s <-> l2.s FROM LSEG_TBL l1, LSEG_TBL l2;

-- Distance to box
SELECT l.s, b.f1, l.s <-> b.f1 AS dist_sb, b.f1 <-> l.s AS dist_bs FROM LSEG_TBL l, BOX_TBL b;

-- Intersect with line segment
SELECT l.s, l1.s FROM LSEG_TBL l, LINE_TBL l1 WHERE l.s ?# l1.s;

-- Intersect with box
SELECT l.s, b.f1 FROM LSEG_TBL l, BOX_TBL b WHERE l.s ?# b.f1;

-- Intersection point with line segment
SELECT l1.s, l2.s, l1.s # l2.s FROM LSEG_TBL l1, LSEG_TBL l2;

-- Closest point to line
SELECT l.s, l1.s, l.s ## l1.s FROM LSEG_TBL l, LINE_TBL l1;

-- Closest point to line segment
SELECT l1.s, l2.s, l1.s ## l2.s FROM LSEG_TBL l1, LSEG_TBL l2;

-- Closest point to box
SELECT l.s, b.f1, l.s ## b.f1 FROM LSEG_TBL l, BOX_TBL b;

-- On line
SELECT l.s, l1.s FROM LSEG_TBL l, LINE_TBL l1 WHERE l.s <@ l1.s;

-- On box
SELECT l.s, b.f1 FROM LSEG_TBL l, BOX_TBL b WHERE l.s <@ b.f1;

--
-- Boxes
--

SELECT '' as six, box(f1) AS box FROM CIRCLE_TBL;

-- translation
SELECT '' AS twentyfour, b.f1 + p.f1 AS translation
   FROM BOX_TBL b, POINT_TBL p;

SELECT '' AS twentyfour, b.f1 - p.f1 AS translation
   FROM BOX_TBL b, POINT_TBL p;

-- Multiply with point
SELECT b.f1, p.f1, b.f1 * p.f1 FROM BOX_TBL b, POINT_TBL p WHERE p.f1[0] BETWEEN 1 AND 1000;

-- Overflow error
SELECT b.f1, p.f1, b.f1 * p.f1 FROM BOX_TBL b, POINT_TBL p WHERE p.f1[0] > 1000;

-- Divide by point
SELECT b.f1, p.f1, b.f1 / p.f1 FROM BOX_TBL b, POINT_TBL p WHERE p.f1[0] BETWEEN 1 AND 1000;

-- To box
SELECT f1::box
	FROM POINT_TBL;

SELECT bound_box(a.f1, b.f1)
	FROM BOX_TBL a, BOX_TBL b;

-- Below box
SELECT b1.f1, b2.f1, b1.f1 <^ b2.f1 FROM BOX_TBL b1, BOX_TBL b2;

-- Above box
SELECT b1.f1, b2.f1, b1.f1 >^ b2.f1 FROM BOX_TBL b1, BOX_TBL b2;

-- Intersection point with box
SELECT b1.f1, b2.f1, b1.f1 # b2.f1 FROM BOX_TBL b1, BOX_TBL b2;

-- Diagonal
SELECT f1, diagonal(f1) FROM BOX_TBL;

-- Distance to box
SELECT b1.f1, b2.f1, b1.f1 <-> b2.f1 FROM BOX_TBL b1, BOX_TBL b2;

--
-- Paths
--

-- Points
SELECT f1, npoints(f1) FROM PATH_TBL;

-- Area
SELECT f1, area(f1) FROM PATH_TBL;

-- Length
SELECT f1, @-@ f1 FROM PATH_TBL;

-- Center
SELECT f1, @@ f1 FROM PATH_TBL;

-- To polygon
SELECT f1, f1::polygon FROM PATH_TBL WHERE isclosed(f1);

-- Open path cannot be converted to polygon error
SELECT f1, f1::polygon FROM PATH_TBL WHERE isopen(f1);

-- Has points less than path
SELECT p1.f1, p2.f1 FROM PATH_TBL p1, PATH_TBL p2 WHERE p1.f1 < p2.f1;

-- Has points less than or equal to path
SELECT p1.f1, p2.f1 FROM PATH_TBL p1, PATH_TBL p2 WHERE p1.f1 <= p2.f1;

-- Has points equal to path
SELECT p1.f1, p2.f1 FROM PATH_TBL p1, PATH_TBL p2 WHERE p1.f1 = p2.f1;

-- Has points greater than or equal to path
SELECT p1.f1, p2.f1 FROM PATH_TBL p1, PATH_TBL p2 WHERE p1.f1 >= p2.f1;

-- Has points greater than path
SELECT p1.f1, p2.f1 FROM PATH_TBL p1, PATH_TBL p2 WHERE p1.f1 > p2.f1;

-- Add path
SELECT p1.f1, p2.f1, p1.f1 + p2.f1 FROM PATH_TBL p1, PATH_TBL p2;

-- Add point
SELECT p.f1, p1.f1, p.f1 + p1.f1 FROM PATH_TBL p, POINT_TBL p1;

-- Subtract point
SELECT p.f1, p1.f1, p.f1 - p1.f1 FROM PATH_TBL p, POINT_TBL p1;

-- Multiply with point
SELECT p.f1, p1.f1, p.f1 * p1.f1 FROM PATH_TBL p, POINT_TBL p1;

-- Divide by point
SELECT p.f1, p1.f1, p.f1 / p1.f1 FROM PATH_TBL p, POINT_TBL p1 WHERE p1.f1[0] BETWEEN 1 AND 1000;

-- Division by 0 error
SELECT p.f1, p1.f1, p.f1 / p1.f1 FROM PATH_TBL p, POINT_TBL p1 WHERE p1.f1 ~= '(0,0)'::point;

-- Distance to path
SELECT p1.f1, p2.f1, p1.f1 <-> p2.f1 FROM PATH_TBL p1, PATH_TBL p2;

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

-- To box
SELECT f1, f1::box FROM POLYGON_TBL;

-- To path
SELECT f1, f1::path FROM POLYGON_TBL;

-- Same as polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 ~= p2.f1;

-- Contained by polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 <@ p2.f1;

-- Contains polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 @> p2.f1;

-- Overlap with polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 && p2.f1;

-- Left of polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 << p2.f1;

-- Overlap of left of polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 &< p2.f1;

-- Right of polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 >> p2.f1;

-- Overlap of right of polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 &> p2.f1;

-- Below polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 <<| p2.f1;

-- Overlap or below polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 &<| p2.f1;

-- Above polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 |>> p2.f1;

-- Overlap or above polygon
SELECT p1.f1, p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2 WHERE p1.f1 |&> p2.f1;

-- Distance to polygon
SELECT p1.f1, p2.f1, p1.f1 <-> p2.f1 FROM POLYGON_TBL p1, POLYGON_TBL p2;

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

-- To polygon
SELECT f1, f1::polygon FROM CIRCLE_TBL WHERE f1 >= '<(0,0),1>';

-- To polygon with less points
SELECT f1, polygon(8, f1) FROM CIRCLE_TBL WHERE f1 >= '<(0,0),1>';

-- Too less points error
SELECT f1, polygon(1, f1) FROM CIRCLE_TBL WHERE f1 >= '<(0,0),1>';

-- Zero radius error
SELECT f1, polygon(10, f1) FROM CIRCLE_TBL WHERE f1 < '<(0,0),1>';

-- Same as circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 ~= c2.f1;

-- Overlap with circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 && c2.f1;

-- Overlap or left of circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 &< c2.f1;

-- Left of circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 << c2.f1;

-- Right of circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 >> c2.f1;

-- Overlap or right of circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 &> c2.f1;

-- Contained by circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 <@ c2.f1;

-- Contain by circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 @> c2.f1;

-- Below circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 <<| c2.f1;

-- Above circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 |>> c2.f1;

-- Overlap or below circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 &<| c2.f1;

-- Overlap or above circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 |&> c2.f1;

-- Area equal with circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 = c2.f1;

-- Area not equal with circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 != c2.f1;

-- Area less than circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 < c2.f1;

-- Area greater than circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 > c2.f1;

-- Area less than or equal circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 <= c2.f1;

-- Area greater than or equal circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 >= c2.f1;

-- Area less than circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 < c2.f1;

-- Area greater than circle
SELECT c1.f1, c2.f1 FROM CIRCLE_TBL c1, CIRCLE_TBL c2 WHERE c1.f1 < c2.f1;

-- Add point
SELECT c.f1, p.f1, c.f1 + p.f1 FROM CIRCLE_TBL c, POINT_TBL p;

-- Subtract point
SELECT c.f1, p.f1, c.f1 - p.f1 FROM CIRCLE_TBL c, POINT_TBL p;

-- Multiply with point
SELECT c.f1, p.f1, c.f1 * p.f1 FROM CIRCLE_TBL c, POINT_TBL p;

-- Divide by point
SELECT c.f1, p.f1, c.f1 / p.f1 FROM CIRCLE_TBL c, POINT_TBL p WHERE p.f1[0] BETWEEN 1 AND 1000;

-- Overflow error
SELECT c.f1, p.f1, c.f1 / p.f1 FROM CIRCLE_TBL c, POINT_TBL p WHERE p.f1[0] > 1000;

-- Division by 0 error
SELECT c.f1, p.f1, c.f1 / p.f1 FROM CIRCLE_TBL c, POINT_TBL p WHERE p.f1 ~= '(0,0)'::point;

-- Distance to polygon
SELECT c.f1, p.f1, c.f1 <-> p.f1 FROM CIRCLE_TBL c, POLYGON_TBL p;
