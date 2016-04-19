--
-- FLOAT8
--

CREATE TABLE FLOAT8_TBL(f1 float8);

INSERT INTO FLOAT8_TBL(f1) VALUES ('    0.0   ');
INSERT INTO FLOAT8_TBL(f1) VALUES ('1004.30  ');
INSERT INTO FLOAT8_TBL(f1) VALUES ('   -34.84');
INSERT INTO FLOAT8_TBL(f1) VALUES ('1.2345678901234e+200');
INSERT INTO FLOAT8_TBL(f1) VALUES ('1.2345678901234e-200');

-- test for underflow and overflow handling
SELECT '10e400'::float8;
SELECT '-10e400'::float8;
SELECT '10e-400'::float8;
SELECT '-10e-400'::float8;

-- bad input
INSERT INTO FLOAT8_TBL(f1) VALUES ('');
INSERT INTO FLOAT8_TBL(f1) VALUES ('     ');
INSERT INTO FLOAT8_TBL(f1) VALUES ('xyz');
INSERT INTO FLOAT8_TBL(f1) VALUES ('5.0.0');
INSERT INTO FLOAT8_TBL(f1) VALUES ('5 . 0');
INSERT INTO FLOAT8_TBL(f1) VALUES ('5.   0');
INSERT INTO FLOAT8_TBL(f1) VALUES ('    - 3');
INSERT INTO FLOAT8_TBL(f1) VALUES ('123           5');

-- special inputs
SELECT 'NaN'::float8;
SELECT 'nan'::float8;
SELECT '   NAN  '::float8;
SELECT 'infinity'::float8;
SELECT '          -INFINiTY   '::float8;
-- bad special inputs
SELECT 'N A N'::float8;
SELECT 'NaN x'::float8;
SELECT ' INFINITY    x'::float8;

SELECT 'Infinity'::float8 + 100.0;
SELECT 'Infinity'::float8 / 'Infinity'::float8;
SELECT 'nan'::float8 / 'nan'::float8;
SELECT 'nan'::numeric::float8;

SELECT '' AS five, * FROM FLOAT8_TBL;

SELECT '' AS four, f.* FROM FLOAT8_TBL f WHERE f.f1 <> '1004.3';

SELECT '' AS one, f.* FROM FLOAT8_TBL f WHERE f.f1 = '1004.3';

SELECT '' AS three, f.* FROM FLOAT8_TBL f WHERE '1004.3' > f.f1;

SELECT '' AS three, f.* FROM FLOAT8_TBL f WHERE  f.f1 < '1004.3';

SELECT '' AS four, f.* FROM FLOAT8_TBL f WHERE '1004.3' >= f.f1;

SELECT '' AS four, f.* FROM FLOAT8_TBL f WHERE  f.f1 <= '1004.3';

SELECT '' AS three, f.f1, f.f1 * '-10' AS x
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS three, f.f1, f.f1 + '-10' AS x
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS three, f.f1, f.f1 / '-10' AS x
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS three, f.f1, f.f1 - '-10' AS x
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS one, f.f1 ^ '2.0' AS square_f1
   FROM FLOAT8_TBL f where f.f1 = '1004.3';

-- absolute value
SELECT '' AS five, f.f1, @f.f1 AS abs_f1
   FROM FLOAT8_TBL f;

-- truncate
SELECT '' AS five, f.f1, trunc(f.f1) AS trunc_f1
   FROM FLOAT8_TBL f;

-- round
SELECT '' AS five, f.f1, round(f.f1) AS round_f1
   FROM FLOAT8_TBL f;

-- ceil / ceiling
select ceil(f1) as ceil_f1 from float8_tbl f;
select ceiling(f1) as ceiling_f1 from float8_tbl f;

-- floor
select floor(f1) as floor_f1 from float8_tbl f;

-- sign
select sign(f1) as sign_f1 from float8_tbl f;

-- square root
SELECT sqrt(float8 '64') AS eight;

SELECT |/ float8 '64' AS eight;

SELECT '' AS three, f.f1, |/f.f1 AS sqrt_f1
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

-- power
SELECT power(float8 '144', float8 '0.5');

-- take exp of ln(f.f1)
SELECT '' AS three, f.f1, exp(ln(f.f1)) AS exp_ln_f1
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

-- cube root
SELECT ||/ float8 '27' AS three;

SELECT '' AS five, f.f1, ||/f.f1 AS cbrt_f1 FROM FLOAT8_TBL f;


SELECT '' AS five, * FROM FLOAT8_TBL;

UPDATE FLOAT8_TBL
   SET f1 = FLOAT8_TBL.f1 * '-1'
   WHERE FLOAT8_TBL.f1 > '0.0';

SELECT '' AS bad, f.f1 * '1e200' from FLOAT8_TBL f;

SELECT '' AS bad, f.f1 ^ '1e200' from FLOAT8_TBL f;

SELECT 0 ^ 0 + 0 ^ 1 + 0 ^ 0.0 + 0 ^ 0.5;

SELECT '' AS bad, ln(f.f1) from FLOAT8_TBL f where f.f1 = '0.0' ;

SELECT '' AS bad, ln(f.f1) from FLOAT8_TBL f where f.f1 < '0.0' ;

SELECT '' AS bad, exp(f.f1) from FLOAT8_TBL f;

SELECT '' AS bad, f.f1 / '0.0' from FLOAT8_TBL f;

SELECT '' AS five, * FROM FLOAT8_TBL;

-- test for over- and underflow
INSERT INTO FLOAT8_TBL(f1) VALUES ('10e400');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-10e400');

INSERT INTO FLOAT8_TBL(f1) VALUES ('10e-400');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-10e-400');

-- maintain external table consistency across platforms
-- delete all values and reinsert well-behaved ones

DELETE FROM FLOAT8_TBL;

INSERT INTO FLOAT8_TBL(f1) VALUES ('0.0');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-34.84');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-1004.30');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-1.2345678901234e+200');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-1.2345678901234e-200');

SELECT '' AS five, * FROM FLOAT8_TBL;

-- test exact cases for trigonometric functions in degrees
SET extra_float_digits = 3;

SELECT x,
       sind(x),
       sind(x) IN (-1,-0.5,0,0.5,1) AS sind_exact
FROM (VALUES (0), (30), (90), (150), (180),
      (210), (270), (330), (360)) AS t(x);

SELECT x,
       cosd(x),
       cosd(x) IN (-1,-0.5,0,0.5,1) AS cosd_exact
FROM (VALUES (0), (60), (90), (120), (180),
      (240), (270), (300), (360)) AS t(x);

SELECT x,
       tand(x),
       tand(x) IN ('-Infinity'::float8,-1,0,
                   1,'Infinity'::float8) AS tand_exact,
       cotd(x),
       cotd(x) IN ('-Infinity'::float8,-1,0,
                   1,'Infinity'::float8) AS cotd_exact
FROM (VALUES (0), (45), (90), (135), (180),
      (225), (270), (315), (360)) AS t(x);

SELECT x,
       asind(x),
       asind(x) IN (-90,-30,0,30,90) AS asind_exact,
       acosd(x),
       acosd(x) IN (0,60,90,120,180) AS acosd_exact
FROM (VALUES (-1), (-0.5), (0), (0.5), (1)) AS t(x);

SELECT x,
       atand(x),
       atand(x) IN (-90,-45,0,45,90) AS atand_exact
FROM (VALUES ('-Infinity'::float8), (-1), (0), (1),
      ('Infinity'::float8)) AS t(x);

SELECT x, y,
       atan2d(y, x),
       atan2d(y, x) IN (-90,0,90,180) AS atan2d_exact
FROM (SELECT 10*cosd(a), 10*sind(a)
      FROM generate_series(0, 360, 90) AS t(a)) AS t(x,y);

RESET extra_float_digits;
