--  *************testing built-in type int4 ****************
--
-- WARNING: int4 operators never check for over/underflow!
-- Some of these answers are consequently numerically incorrect.
--

CREATE TABLE INT4_TBL(f1 int4);

INSERT INTO INT4_TBL(f1) VALUES ('0');

INSERT INTO INT4_TBL(f1) VALUES ('123456');

INSERT INTO INT4_TBL(f1) VALUES ('-123456');

INSERT INTO INT4_TBL(f1) VALUES ('34.5');

-- largest and smallest values 
INSERT INTO INT4_TBL(f1) VALUES ('2147483647');

INSERT INTO INT4_TBL(f1) VALUES ('-2147483647');

-- bad input values -- should give warnings 
INSERT INTO INT4_TBL(f1) VALUES ('1000000000000');

INSERT INTO INT4_TBL(f1) VALUES ('asdf');


SELECT '' AS five, INT4_TBL.*;

SELECT '' AS four, i.* FROM INT4_TBL i WHERE i.f1 <> '0'::int2;

SELECT '' AS four, i.* FROM INT4_TBL i WHERE i.f1 <> '0'::int4;

SELECT '' AS one, i.* FROM INT4_TBL i WHERE i.f1 = '0'::int2;

SELECT '' AS one, i.* FROM INT4_TBL i WHERE i.f1 = '0'::int4;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 < '0'::int2;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 < '0'::int4;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 <= '0'::int2;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 <= '0'::int4;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 > '0'::int2;

SELECT '' AS two, i.* FROM INT4_TBL i WHERE i.f1 > '0'::int4;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 >= '0'::int2;

SELECT '' AS three, i.* FROM INT4_TBL i WHERE i.f1 >= '0'::int4;

-- positive odds 
SELECT '' AS one, i.* FROM INT4_TBL i WHERE (i.f1 % '2'::int2) = '1'::int2;

-- any evens 
SELECT '' AS three, i.* FROM INT4_TBL i WHERE (i.f1 % '2'::int4) = '0'::int2;

SELECT '' AS five, i.f1, i.f1 * '2'::int2 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 * '2'::int4 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 + '2'::int2 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 + '2'::int4 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 - '2'::int2 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 - '2'::int4 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 / '2'::int2 AS x FROM INT4_TBL i;

SELECT '' AS five, i.f1, i.f1 / '2'::int4 AS x FROM INT4_TBL i;

--
-- more complex expressions
--

-- variations on unary minus parsing
SELECT -2+3 AS one;

SELECT 4-2 AS two;

SELECT 2- -1 AS three;

SELECT 2 - -2 AS four;

SELECT '2'::int2 * '2'::int2 = '16'::int2 / '4'::int2 AS true;

SELECT '2'::int4 * '2'::int2 = '16'::int2 / '4'::int4 AS true;

SELECT '2'::int2 * '2'::int4 = '16'::int4 / '4'::int2 AS true;

SELECT '1000'::int4 < '999'::int4 AS false;

SELECT 4! AS twenty_four;

SELECT !!3 AS six;

SELECT 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 AS ten;

SELECT 2 + 2 / 2 AS three;

SELECT (2 + 2) / 2 AS two;

SELECT dsqrt('64'::float8) AS eight;

SELECT |/'64'::float8 AS eight;

SELECT ||/'27'::float8 AS three;

