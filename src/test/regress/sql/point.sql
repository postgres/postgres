--  ************testing built-in type point ****************

CREATE TABLE POINT_TBL(f1 point);

INSERT INTO POINT_TBL(f1) VALUES ('(0.0,0.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(-10.0,0.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(-3.0,4.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(5.1, 34.5)');

INSERT INTO POINT_TBL(f1) VALUES ('(-5.0,-12.0)');

-- bad format points 
INSERT INTO POINT_TBL(f1) VALUES ('asdfasdf');

INSERT INTO POINT_TBL(f1) VALUES ('10.0,10.0');

INSERT INTO POINT_TBL(f1) VALUES ('(10.0 10.0)');

INSERT INTO POINT_TBL(f1) VALUES ('(10.0,10.0');


SELECT '' AS six, POINT_TBL.*;

-- left of 
SELECT '' AS three, p.* FROM POINT_TBL p WHERE p.f1 !< '(0.0, 0.0)';

-- right of 
SELECT '' AS three, p.* FROM POINT_TBL p WHERE '(0.0,0.0)' !> p.f1;

-- above 
SELECT '' AS one, p.* FROM POINT_TBL p WHERE '(0.0,0.0)' !^ p.f1;

-- below 
SELECT '' AS one, p.* FROM POINT_TBL p WHERE p.f1 !| '(0.0, 0.0)';

-- equal 
SELECT '' AS one, p.* FROM POINT_TBL p WHERE p.f1 =|= '(5.1, 34.5)';

-- point in box 
SELECT '' AS three, p.* FROM POINT_TBL p
   WHERE p.f1 ===> '(0,0,100,100)';

SELECT '' AS three, p.* FROM POINT_TBL p
   WHERE not on_pb(p.f1,'(0,0,100,100)'::box);

SELECT '' AS two, p.* FROM POINT_TBL p
   WHERE on_ppath(p.f1,'(0,3,0,0,-10,0,-10,10)'::path);

SELECT '' AS six, p.f1, p.f1 <===> '(0,0)'::point AS dist FROM POINT_TBL p;

SELECT '' AS thirtysix, p1.f1, p2.f1, p1.f1 <===> p2.f1 AS dist
   FROM POINT_TBL p1, POINT_TBL p2;

SELECT '' AS thirty, p1.f1, p2.f1
   FROM POINT_TBL p1, POINT_TBL p2
   WHERE (p1.f1 <===> p2.f1) > 3;

SELECT '' AS fifteen, p1.f1, p2.f1
   FROM POINT_TBL p1, POINT_TBL p2
   WHERE (p1.f1 <===> p2.f1) > 3 and 
	p1.f1 !< p2.f1;

SELECT '' AS three, p1.f1, p2.f1 
   FROM POINT_TBL p1, POINT_TBL p2 
   WHERE (p1.f1 <===> p2.f1) > 3 and 
	p1.f1 !< p2.f1 and
	p1.f1 !^ p2.f1;

DROP TABLE  POINT_TBL;
