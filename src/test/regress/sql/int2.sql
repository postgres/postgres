--
-- INT2
-- NOTE: int2 operators never check for over/underflow!
-- Some of these answers are consequently numerically incorrect.
--

CREATE TABLE INT2_TBL(f1 int2);

INSERT INTO INT2_TBL(f1) VALUES ('0');

INSERT INTO INT2_TBL(f1) VALUES ('1234');

INSERT INTO INT2_TBL(f1) VALUES ('-1234');

INSERT INTO INT2_TBL(f1) VALUES ('34.5');

-- largest and smallest values 
INSERT INTO INT2_TBL(f1) VALUES ('32767');

INSERT INTO INT2_TBL(f1) VALUES ('-32767');

-- bad input values -- should give warnings 
INSERT INTO INT2_TBL(f1) VALUES ('100000');

INSERT INTO INT2_TBL(f1) VALUES ('asdf');


SELECT '' AS five, INT2_TBL.*;

SELECT '' AS four, i.* FROM INT2_TBL i WHERE i.f1 <> int2 '0';

SELECT '' AS four, i.* FROM INT2_TBL i WHERE i.f1 <> int4 '0';

SELECT '' AS one, i.* FROM INT2_TBL i WHERE i.f1 = int2 '0';

SELECT '' AS one, i.* FROM INT2_TBL i WHERE i.f1 = int4 '0';

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 < int2 '0';

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 < int4 '0';

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 <= int2 '0';

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 <= int4 '0';

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 > int2 '0';

SELECT '' AS two, i.* FROM INT2_TBL i WHERE i.f1 > int4 '0';

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 >= int2 '0';

SELECT '' AS three, i.* FROM INT2_TBL i WHERE i.f1 >= int4 '0';

-- positive odds 
SELECT '' AS one, i.* FROM INT2_TBL i WHERE (i.f1 % int2 '2') = int2 '1';

-- any evens 
SELECT '' AS three, i.* FROM INT2_TBL i WHERE (i.f1 % int4 '2') = int2 '0';

SELECT '' AS five, i.f1, i.f1 * int2 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 * int4 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 + int2 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 + int4 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 - int2 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 - int4 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 / int2 '2' AS x FROM INT2_TBL i;

SELECT '' AS five, i.f1, i.f1 / int4 '2' AS x FROM INT2_TBL i;

