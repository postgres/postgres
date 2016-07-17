--
-- CREATE_INDEX
-- Create ancillary data structures (i.e. indices)
--

--
-- BTREE
--
CREATE INDEX onek_unique1 ON onek USING btree(unique1 int4_ops);

CREATE INDEX IF NOT EXISTS onek_unique1 ON onek USING btree(unique1 int4_ops);

CREATE INDEX IF NOT EXISTS ON onek USING btree(unique1 int4_ops);

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

INSERT INTO POINT_TBL(f1) VALUES (NULL);

CREATE INDEX gpointind ON point_tbl USING gist (f1);

CREATE TEMP TABLE gpolygon_tbl AS
    SELECT polygon(home_base) AS f1 FROM slow_emp4000;
INSERT INTO gpolygon_tbl VALUES ( '(1000,0,0,1000)' );
INSERT INTO gpolygon_tbl VALUES ( '(0,1000,1000,1000)' );

CREATE TEMP TABLE gcircle_tbl AS
    SELECT circle(home_base) AS f1 FROM slow_emp4000;

CREATE INDEX ggpolygonind ON gpolygon_tbl USING gist (f1);

CREATE INDEX ggcircleind ON gcircle_tbl USING gist (f1);

--
-- SP-GiST
--

CREATE TABLE quad_point_tbl AS
    SELECT point(unique1,unique2) AS p FROM tenk1;

INSERT INTO quad_point_tbl
    SELECT '(333.0,400.0)'::point FROM generate_series(1,1000);

INSERT INTO quad_point_tbl VALUES (NULL), (NULL), (NULL);

CREATE INDEX sp_quad_ind ON quad_point_tbl USING spgist (p);

CREATE TABLE kd_point_tbl AS SELECT * FROM quad_point_tbl;

CREATE INDEX sp_kd_ind ON kd_point_tbl USING spgist (p kd_point_ops);

CREATE TABLE radix_text_tbl AS
    SELECT name AS t FROM road WHERE name !~ '^[0-9]';

INSERT INTO radix_text_tbl
    SELECT 'P0123456789abcdef' FROM generate_series(1,1000);
INSERT INTO radix_text_tbl VALUES ('P0123456789abcde');
INSERT INTO radix_text_tbl VALUES ('P0123456789abcdefF');

CREATE INDEX sp_radix_ind ON radix_text_tbl USING spgist (t);

--
-- Test GiST and SP-GiST indexes
--

-- get non-indexed results for comparison purposes

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

SELECT * FROM point_tbl ORDER BY f1 <-> '0,1';

SELECT * FROM point_tbl WHERE f1 IS NULL;

SELECT * FROM point_tbl WHERE f1 IS NOT NULL ORDER BY f1 <-> '0,1';

SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';

SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

SELECT count(*) FROM quad_point_tbl;

SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p <^ '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p >^ '(5000, 4000)';

SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

SELECT * FROM gpolygon_tbl ORDER BY f1 <-> '(0,0)'::point LIMIT 10;

SELECT circle_center(f1), round(radius(f1)) as radius FROM gcircle_tbl ORDER BY f1 <-> '(200,300)'::point LIMIT 10;

-- Now check the results from plain indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

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

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 IS NULL;
SELECT * FROM point_tbl WHERE f1 IS NULL;

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 IS NOT NULL ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl WHERE f1 IS NOT NULL ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl;
SELECT count(*) FROM quad_point_tbl;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <^ '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p <^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p >^ '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p >^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <^ '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p <^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p >^ '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p >^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT * FROM gpolygon_tbl ORDER BY f1 <-> '(0,0)'::point LIMIT 10;
SELECT * FROM gpolygon_tbl ORDER BY f1 <-> '(0,0)'::point LIMIT 10;

EXPLAIN (COSTS OFF)
SELECT circle_center(f1), round(radius(f1)) as radius FROM gcircle_tbl ORDER BY f1 <-> '(200,300)'::point LIMIT 10;
SELECT circle_center(f1), round(radius(f1)) as radius FROM gcircle_tbl ORDER BY f1 <-> '(200,300)'::point LIMIT 10;

-- Now check the results from bitmap indexscan
SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

EXPLAIN (COSTS OFF)
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';
SELECT * FROM point_tbl WHERE f1 <@ '(-10,-10),(10,10)':: box ORDER BY f1 <-> '0,1';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;
SELECT count(*) FROM quad_point_tbl WHERE p IS NOT NULL;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl;
SELECT count(*) FROM quad_point_tbl;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM quad_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM quad_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p <^ '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p <^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p >^ '(5000, 4000)';
SELECT count(*) FROM quad_point_tbl WHERE p >^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM quad_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';
SELECT count(*) FROM kd_point_tbl WHERE p <@ box '(200,200,1000,1000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;
SELECT count(*) FROM kd_point_tbl WHERE box '(200,200,1000,1000)' @> p;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p << '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p >> '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p <^ '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p <^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p >^ '(5000, 4000)';
SELECT count(*) FROM kd_point_tbl WHERE p >^ '(5000, 4000)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';
SELECT count(*) FROM kd_point_tbl WHERE p ~= '(4585, 365)';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdef';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcde';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';
SELECT count(*) FROM radix_text_tbl WHERE t = 'P0123456789abcdefF';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<~  'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t <=   'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~<=~ 'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Aztec                         Ct  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t =    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >=   'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>=~ 'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t >    'Worth                         St  ';

EXPLAIN (COSTS OFF)
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';
SELECT count(*) FROM radix_text_tbl WHERE t ~>~  'Worth                         St  ';

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--
-- GIN over int[] and text[]
--
-- Note: GIN currently supports only bitmap scans, not plain indexscans
--

SET enable_seqscan = OFF;
SET enable_indexscan = OFF;
SET enable_bitmapscan = ON;

CREATE INDEX intarrayidx ON array_index_op_test USING gin (i);

explain (costs off)
SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32,17}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{38,34,32,89}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i = '{47,77}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i = '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i <@ '{}' ORDER BY seqno;
SELECT * FROM array_op_test WHERE i = '{NULL}' ORDER BY seqno;
SELECT * FROM array_op_test WHERE i @> '{NULL}' ORDER BY seqno;
SELECT * FROM array_op_test WHERE i && '{NULL}' ORDER BY seqno;
SELECT * FROM array_op_test WHERE i <@ '{NULL}' ORDER BY seqno;

CREATE INDEX textarrayidx ON array_index_op_test USING gin (t);

explain (costs off)
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908}' ORDER BY seqno;

SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAA72908}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAAA72908,AAAAAAAAAA646}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t <@ '{AAAAAAAA72908,AAAAAAAAAAAAAAAAAAA17075,AA88409,AAAAAAAAAAAAAAAAAA36842,AAAAAAA48038,AAAAAAAAAAAAAA10611}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{AAAAAAAAAA646,A87088}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t <@ '{}' ORDER BY seqno;

-- And try it with a multicolumn GIN index

DROP INDEX intarrayidx, textarrayidx;

CREATE INDEX botharrayidx ON array_index_op_test USING gin (i, t);

SELECT * FROM array_index_op_test WHERE i @> '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t @> '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i @> '{32}' AND t && '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE i && '{32}' AND t @> '{AAAAAAA80240}' ORDER BY seqno;
SELECT * FROM array_index_op_test WHERE t = '{}' ORDER BY seqno;
SELECT * FROM array_op_test WHERE i = '{NULL}' ORDER BY seqno;
SELECT * FROM array_op_test WHERE i <@ '{NULL}' ORDER BY seqno;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--
-- Try a GIN index with a lot of items with same key. (GIN creates a posting
-- tree when there are enough duplicates)
--
CREATE TABLE array_gin_test (a int[]);

INSERT INTO array_gin_test SELECT ARRAY[1, g%5, g] FROM generate_series(1, 10000) g;

CREATE INDEX array_gin_test_idx ON array_gin_test USING gin (a);

SELECT COUNT(*) FROM array_gin_test WHERE a @> '{2}';

DROP TABLE array_gin_test;

--
-- Test GIN index's reloptions
--
CREATE INDEX gin_relopts_test ON array_index_op_test USING gin (i)
  WITH (FASTUPDATE=on, GIN_PENDING_LIST_LIMIT=128);
\d+ gin_relopts_test

--
-- HASH
--
CREATE INDEX hash_i4_index ON hash_i4_heap USING hash (random int4_ops);

CREATE INDEX hash_name_index ON hash_name_heap USING hash (random name_ops);

CREATE INDEX hash_txt_index ON hash_txt_heap USING hash (random text_ops);

CREATE INDEX hash_f8_index ON hash_f8_heap USING hash (random float8_ops);

CREATE UNLOGGED TABLE unlogged_hash_table (id int4);
CREATE INDEX unlogged_hash_index ON unlogged_hash_table USING hash (id int4_ops);
DROP TABLE unlogged_hash_table;

-- CREATE INDEX hash_ovfl_index ON hash_ovfl_heap USING hash (x int4_ops);

-- Test hash index build tuplesorting.  Force hash tuplesort using low
-- maintenance_work_mem setting and fillfactor:
SET maintenance_work_mem = '1MB';
CREATE INDEX hash_tuplesort_idx ON tenk1 USING hash (stringu1 name_ops) WITH (fillfactor = 10);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1 WHERE stringu1 = 'TVAAAA';
SELECT count(*) FROM tenk1 WHERE stringu1 = 'TVAAAA';
DROP INDEX hash_tuplesort_idx;
RESET maintenance_work_mem;


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
CREATE INDEX CONCURRENTLY IF NOT EXISTS concur_index1 ON concur_heap(f2,f1);
INSERT INTO concur_heap VALUES  ('a','b');
INSERT INTO concur_heap VALUES  ('b','b');
-- unique index
CREATE UNIQUE INDEX CONCURRENTLY concur_index2 ON concur_heap(f1);
CREATE UNIQUE INDEX CONCURRENTLY IF NOT EXISTS concur_index2 ON concur_heap(f1);
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

-- Failed builds are left invalid by VACUUM FULL, fixed by REINDEX
VACUUM FULL concur_heap;
REINDEX TABLE concur_heap;
DELETE FROM concur_heap WHERE f1 = 'b';
VACUUM FULL concur_heap;
\d concur_heap
REINDEX TABLE concur_heap;
\d concur_heap

--
-- Try some concurrent index drops
--
DROP INDEX CONCURRENTLY "concur_index2";				-- works
DROP INDEX CONCURRENTLY IF EXISTS "concur_index2";		-- notice

-- failures
DROP INDEX CONCURRENTLY "concur_index2", "concur_index3";
BEGIN;
DROP INDEX CONCURRENTLY "concur_index5";
ROLLBACK;

-- successes
DROP INDEX CONCURRENTLY IF EXISTS "concur_index3";
DROP INDEX CONCURRENTLY "concur_index4";
DROP INDEX CONCURRENTLY "concur_index5";
DROP INDEX CONCURRENTLY "concur_index1";
DROP INDEX CONCURRENTLY "concur_heap_expr_idx";

\d concur_heap

DROP TABLE concur_heap;

--
-- Test ADD CONSTRAINT USING INDEX
--

CREATE TABLE cwi_test( a int , b varchar(10), c char);

-- add some data so that all tests have something to work with.

INSERT INTO cwi_test VALUES(1, 2), (3, 4), (5, 6);

CREATE UNIQUE INDEX cwi_uniq_idx ON cwi_test(a , b);
ALTER TABLE cwi_test ADD primary key USING INDEX cwi_uniq_idx;

\d cwi_test
\d cwi_uniq_idx

CREATE UNIQUE INDEX cwi_uniq2_idx ON cwi_test(b , a);
ALTER TABLE cwi_test DROP CONSTRAINT cwi_uniq_idx,
	ADD CONSTRAINT cwi_replaced_pkey PRIMARY KEY
		USING INDEX cwi_uniq2_idx;

\d cwi_test
\d cwi_replaced_pkey

DROP INDEX cwi_replaced_pkey;	-- Should fail; a constraint depends on it

DROP TABLE cwi_test;

--
-- Check handling of indexes on system columns
--
CREATE TABLE oid_table (a INT) WITH OIDS;

-- An index on the OID column should be allowed
CREATE INDEX ON oid_table (oid);

-- Other system columns cannot be indexed
CREATE INDEX ON oid_table (ctid);

-- nor used in expressions
CREATE INDEX ON oid_table ((ctid >= '(1000,0)'));

-- nor used in predicates
CREATE INDEX ON oid_table (a) WHERE ctid >= '(1000,0)';

DROP TABLE oid_table;

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
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2 desc,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2 desc nulls last,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;

DROP INDEX onek_nulltest;

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2  nulls first,unique1);

SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique2 IS NOT NULL;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NOT NULL AND unique1 > 500;
SELECT count(*) FROM onek_with_null WHERE unique1 IS NULL AND unique1 > 500;

DROP INDEX onek_nulltest;

-- Check initial-positioning logic too

CREATE UNIQUE INDEX onek_nulltest ON onek_with_null (unique2);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= -1
  ORDER BY unique2 LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= 0
  ORDER BY unique2 LIMIT 2;

SELECT unique1, unique2 FROM onek_with_null
  ORDER BY unique2 DESC LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 >= -1
  ORDER BY unique2 DESC LIMIT 2;
SELECT unique1, unique2 FROM onek_with_null WHERE unique2 < 999
  ORDER BY unique2 DESC LIMIT 2;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

DROP TABLE onek_with_null;

--
-- Check bitmap index path planning
--

EXPLAIN (COSTS OFF)
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42);
SELECT * FROM tenk1
  WHERE thousand = 42 AND (tenthous = 1 OR tenthous = 3 OR tenthous = 42);

EXPLAIN (COSTS OFF)
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99);
SELECT count(*) FROM tenk1
  WHERE hundred = 42 AND (thousand = 42 OR thousand = 99);

--
-- Check behavior with duplicate index column contents
--

CREATE TABLE dupindexcols AS
  SELECT unique1 as id, stringu2::text as f1 FROM tenk1;
CREATE INDEX dupindexcols_i ON dupindexcols (f1, id, f1 text_pattern_ops);
ANALYZE dupindexcols;

EXPLAIN (COSTS OFF)
  SELECT count(*) FROM dupindexcols
    WHERE f1 BETWEEN 'WA' AND 'ZZZ' and id < 1000 and f1 ~<~ 'YX';
SELECT count(*) FROM dupindexcols
  WHERE f1 BETWEEN 'WA' AND 'ZZZ' and id < 1000 and f1 ~<~ 'YX';

--
-- Check ordering of =ANY indexqual results (bug in 9.2.0)
--

vacuum tenk1;		-- ensure we get consistent plans here

explain (costs off)
SELECT unique1 FROM tenk1
WHERE unique1 IN (1,42,7)
ORDER BY unique1;

SELECT unique1 FROM tenk1
WHERE unique1 IN (1,42,7)
ORDER BY unique1;

explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand;

SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand;

SET enable_indexonlyscan = OFF;

explain (costs off)
SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand;

SELECT thousand, tenthous FROM tenk1
WHERE thousand < 2 AND tenthous IN (1001,3000)
ORDER BY thousand;

RESET enable_indexscan;

--
-- Check elimination of constant-NULL subexpressions
--

explain (costs off)
  select * from tenk1 where (thousand, tenthous) in ((1,1001), (null,null));

--
-- REINDEX (VERBOSE)
--
CREATE TABLE reindex_verbose(id integer primary key);
\set VERBOSITY terse
REINDEX (VERBOSE) TABLE reindex_verbose;
DROP TABLE reindex_verbose;

--
-- REINDEX SCHEMA
--
REINDEX SCHEMA schema_to_reindex; -- failure, schema does not exist
CREATE SCHEMA schema_to_reindex;
SET search_path = 'schema_to_reindex';
CREATE TABLE table1(col1 SERIAL PRIMARY KEY);
INSERT INTO table1 SELECT generate_series(1,400);
CREATE TABLE table2(col1 SERIAL PRIMARY KEY, col2 TEXT NOT NULL);
INSERT INTO table2 SELECT generate_series(1,400), 'abc';
CREATE INDEX ON table2(col2);
CREATE MATERIALIZED VIEW matview AS SELECT col1 FROM table2;
CREATE INDEX ON matview(col1);
CREATE VIEW view AS SELECT col2 FROM table2;
CREATE TABLE reindex_before AS
SELECT oid, relname, relfilenode, relkind, reltoastrelid
	FROM pg_class
	where relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'schema_to_reindex');
INSERT INTO reindex_before
SELECT oid, 'pg_toast_TABLE', relfilenode, relkind, reltoastrelid
FROM pg_class WHERE oid IN
	(SELECT reltoastrelid FROM reindex_before WHERE reltoastrelid > 0);
INSERT INTO reindex_before
SELECT oid, 'pg_toast_TABLE_index', relfilenode, relkind, reltoastrelid
FROM pg_class where oid in
	(select indexrelid from pg_index where indrelid in
		(select reltoastrelid from reindex_before where reltoastrelid > 0));
REINDEX SCHEMA schema_to_reindex;
CREATE TABLE reindex_after AS SELECT oid, relname, relfilenode, relkind
	FROM pg_class
	where relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'schema_to_reindex');
SELECT  b.relname,
        b.relkind,
        CASE WHEN a.relfilenode = b.relfilenode THEN 'relfilenode is unchanged'
        ELSE 'relfilenode has changed' END
  FROM reindex_before b JOIN pg_class a ON b.oid = a.oid
  ORDER BY 1;
REINDEX SCHEMA schema_to_reindex;
BEGIN;
REINDEX SCHEMA schema_to_reindex; -- failure, cannot run in a transaction
END;

-- Failure for unauthorized user
CREATE ROLE regress_reindexuser NOLOGIN;
SET SESSION ROLE regress_reindexuser;
REINDEX SCHEMA schema_to_reindex;

-- Clean up
RESET ROLE;
DROP ROLE regress_reindexuser;
SET client_min_messages TO 'warning';
DROP SCHEMA schema_to_reindex CASCADE;
RESET client_min_messages;
