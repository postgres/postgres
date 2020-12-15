--
-- CIRCLE
--

-- Back off displayed precision a little bit to reduce platform-to-platform
-- variation in results.
SET extra_float_digits = -1;

CREATE TABLE CIRCLE_TBL (f1 circle);

INSERT INTO CIRCLE_TBL VALUES ('<(5,1),3>');

INSERT INTO CIRCLE_TBL VALUES ('((1,2),100)');

INSERT INTO CIRCLE_TBL VALUES (' 1 , 3 , 5 ');

INSERT INTO CIRCLE_TBL VALUES (' ( ( 1 , 2 ) , 3 ) ');

INSERT INTO CIRCLE_TBL VALUES (' ( 100 , 200 ) , 10 ');

INSERT INTO CIRCLE_TBL VALUES (' < ( 100 , 1 ) , 115 > ');

INSERT INTO CIRCLE_TBL VALUES ('<(3,5),0>');	-- Zero radius

INSERT INTO CIRCLE_TBL VALUES ('<(3,5),NaN>');	-- NaN radius

-- bad values

INSERT INTO CIRCLE_TBL VALUES ('<(-100,0),-100>');

INSERT INTO CIRCLE_TBL VALUES ('<(100,200),10');

INSERT INTO CIRCLE_TBL VALUES ('<(100,200),10> x');

INSERT INTO CIRCLE_TBL VALUES ('1abc,3,5');

INSERT INTO CIRCLE_TBL VALUES ('(3,(1,2),3)');

SELECT * FROM CIRCLE_TBL;

SELECT center(f1) AS center
  FROM CIRCLE_TBL;

SELECT radius(f1) AS radius
  FROM CIRCLE_TBL;

SELECT diameter(f1) AS diameter
  FROM CIRCLE_TBL;

SELECT f1 FROM CIRCLE_TBL WHERE radius(f1) < 5;

SELECT f1 FROM CIRCLE_TBL WHERE diameter(f1) >= 10;

SELECT c1.f1 AS one, c2.f1 AS two, (c1.f1 <-> c2.f1) AS distance
  FROM CIRCLE_TBL c1, CIRCLE_TBL c2
  WHERE (c1.f1 < c2.f1) AND ((c1.f1 <-> c2.f1) > 0)
  ORDER BY distance, area(c1.f1), area(c2.f1);
