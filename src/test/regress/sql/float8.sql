--
-- FLOAT8
--

CREATE TABLE FLOAT8_TBL(f1 float8);

INSERT INTO FLOAT8_TBL(f1) VALUES ('0.0');

INSERT INTO FLOAT8_TBL(f1) VALUES ('1004.30');

INSERT INTO FLOAT8_TBL(f1) VALUES ('-34.84');

INSERT INTO FLOAT8_TBL(f1) VALUES ('1.2345678901234e+200');

INSERT INTO FLOAT8_TBL(f1) VALUES ('1.2345678901234e-200');


SELECT '' AS five, FLOAT8_TBL.*;

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
SELECT '' AS five, f.f1, %f.f1 AS trunc_f1
   FROM FLOAT8_TBL f;

-- round 
SELECT '' AS five, f.f1, f.f1 % AS round_f1
   FROM FLOAT8_TBL f;

-- square root 
SELECT '' AS three, f.f1, |/f.f1 AS sqrt_f1
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

-- take exp of ln(f.f1) 
SELECT '' AS three, f.f1, : ( ; f.f1) AS exp_ln_f1
   FROM FLOAT8_TBL f
   WHERE f.f1 > '0.0';

-- cube root 
SELECT '' AS five, f.f1, ||/f.f1 AS cbrt_f1 FROM FLOAT8_TBL f;


SELECT '' AS five, FLOAT8_TBL.*;

UPDATE FLOAT8_TBL
   SET f1 = FLOAT8_TBL.f1 * '-1'
   WHERE FLOAT8_TBL.f1 > '0.0';

SELECT '' AS bad, f.f1 * '1e200' from FLOAT8_TBL f;

SELECT '' AS bad, f.f1 ^ '1e200' from FLOAT8_TBL f;

SELECT '' AS bad, (; (f.f1)) from FLOAT8_TBL f where f.f1 = '0.0' ;

SELECT '' AS bad, (; (f.f1)) from FLOAT8_TBL f where f.f1 < '0.0' ;

SELECT '' AS bad, : (f.f1) from FLOAT8_TBL f;

SELECT '' AS bad, f.f1 / '0.0' from FLOAT8_TBL f;

SELECT '' AS five, FLOAT8_TBL.*;

-- test for over and under flow 
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

SELECT '' AS five, FLOAT8_TBL.*;

