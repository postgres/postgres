--
-- POINT
--

-- avoid bit-exact output here because operations may not be bit-exact.
SET extra_float_digits = 0;

CREATE TABLE POINT_TBL(f1 point);

INSERT INTO POINT_TBL(f1) VALUES ('(0.0,0.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(-10.0,0.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(-3.0,4.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(5.1, 34.5)');

INSERT INTO POINT_TBL(f1) VALUES ('(-5.0,-12.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(1e-300,-1e-300)');	-- To underflow

INSERT INTO POINT_TBL(f1) VALUES ('(1e+300,Inf)');		-- To overflow

INSERT INTO POINT_TBL(f1) VALUES ('(Inf,1e+300)');		-- Transposed

INSERT INTO POINT_TBL(f1) VALUES (' ( Nan , NaN ) ');

-- bad format points
INSERT INTO POINT_TBL(f1) VALUES ('asdfasdf');

INSERT INTO POINT_TBL(f1) VALUES ('10.0,10.0');

INSERT INTO POINT_TBL(f1) VALUES ('(10.0 10.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(10.0, 10.0) x');

INSERT INTO POINT_TBL(f1) VALUES ('(10.0,10.0');

INSERT INTO POINT_TBL(f1) VALUES ('(10.0, 1e+500)');	-- Out of range


SELECT * FROM POINT_TBL;

-- left of
SELECT p.* FROM POINT_TBL p WHERE p.f1 << '(0.0, 0.0)';

-- right of
SELECT p.* FROM POINT_TBL p WHERE '(0.0,0.0)' >> p.f1;

-- above
SELECT p.* FROM POINT_TBL p WHERE '(0.0,0.0)' |>> p.f1;

-- below
SELECT p.* FROM POINT_TBL p WHERE p.f1 <<| '(0.0, 0.0)';

-- equal
SELECT p.* FROM POINT_TBL p WHERE p.f1 ~= '(5.1, 34.5)';

-- point in box
SELECT p.* FROM POINT_TBL p
   WHERE p.f1 <@ box '(0,0,100,100)';

SELECT p.* FROM POINT_TBL p
   WHERE box '(0,0,100,100)' @> p.f1;

SELECT p.* FROM POINT_TBL p
   WHERE not p.f1 <@ box '(0,0,100,100)';

SELECT p.* FROM POINT_TBL p
   WHERE p.f1 <@ path '[(0,0),(-10,0),(-10,10)]';

SELECT p.* FROM POINT_TBL p
   WHERE not box '(0,0,100,100)' @> p.f1;

SELECT p.f1, p.f1 <-> point '(0,0)' AS dist
   FROM POINT_TBL p
   ORDER BY dist;

SELECT p1.f1 AS point1, p2.f1 AS point2, p1.f1 <-> p2.f1 AS dist
   FROM POINT_TBL p1, POINT_TBL p2
   ORDER BY dist, p1.f1[0], p2.f1[0];

SELECT p1.f1 AS point1, p2.f1 AS point2
   FROM POINT_TBL p1, POINT_TBL p2
   WHERE (p1.f1 <-> p2.f1) > 3;

-- put distance result into output to allow sorting with GEQ optimizer - tgl 97/05/10
SELECT p1.f1 AS point1, p2.f1 AS point2, (p1.f1 <-> p2.f1) AS distance
   FROM POINT_TBL p1, POINT_TBL p2
   WHERE (p1.f1 <-> p2.f1) > 3 and p1.f1 << p2.f1
   ORDER BY distance, p1.f1[0], p2.f1[0];

-- put distance result into output to allow sorting with GEQ optimizer - tgl 97/05/10
SELECT p1.f1 AS point1, p2.f1 AS point2, (p1.f1 <-> p2.f1) AS distance
   FROM POINT_TBL p1, POINT_TBL p2
   WHERE (p1.f1 <-> p2.f1) > 3 and p1.f1 << p2.f1 and p1.f1 |>> p2.f1
   ORDER BY distance;

-- Test that GiST indexes provide same behavior as sequential scan
CREATE TEMP TABLE point_gist_tbl(f1 point);
INSERT INTO point_gist_tbl SELECT '(0,0)' FROM generate_series(0,1000);
CREATE INDEX point_gist_tbl_index ON point_gist_tbl USING gist (f1);
INSERT INTO point_gist_tbl VALUES ('(0.0000009,0.0000009)');
SET enable_seqscan TO true;
SET enable_indexscan TO false;
SET enable_bitmapscan TO false;
SELECT COUNT(*) FROM point_gist_tbl WHERE f1 ~= '(0.0000009,0.0000009)'::point;
SELECT COUNT(*) FROM point_gist_tbl WHERE f1 <@ '(0.0000009,0.0000009),(0.0000009,0.0000009)'::box;
SELECT COUNT(*) FROM point_gist_tbl WHERE f1 ~= '(0.0000018,0.0000018)'::point;
SET enable_seqscan TO false;
SET enable_indexscan TO true;
SET enable_bitmapscan TO true;
SELECT COUNT(*) FROM point_gist_tbl WHERE f1 ~= '(0.0000009,0.0000009)'::point;
SELECT COUNT(*) FROM point_gist_tbl WHERE f1 <@ '(0.0000009,0.0000009),(0.0000009,0.0000009)'::box;
SELECT COUNT(*) FROM point_gist_tbl WHERE f1 ~= '(0.0000018,0.0000018)'::point;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
