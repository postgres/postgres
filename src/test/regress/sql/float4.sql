--
-- FLOAT4
--

CREATE TABLE FLOAT4_TBL (f1  float4);

INSERT INTO FLOAT4_TBL(f1) VALUES ('0.0');

INSERT INTO FLOAT4_TBL(f1) VALUES ('1004.30');

INSERT INTO FLOAT4_TBL(f1) VALUES ('-34.84');

INSERT INTO FLOAT4_TBL(f1) VALUES ('1.2345678901234e+20');

INSERT INTO FLOAT4_TBL(f1) VALUES ('1.2345678901234e-20');

-- test for over and under flow 
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e40');

INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e40');

INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-40');

INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-40');


SELECT '' AS five, FLOAT4_TBL.*;

SELECT '' AS four, f.* FROM FLOAT4_TBL f WHERE f.f1 <> '1004.3';

SELECT '' AS one, f.* FROM FLOAT4_TBL f WHERE f.f1 = '1004.3';

SELECT '' AS three, f.* FROM FLOAT4_TBL f WHERE '1004.3' > f.f1;

SELECT '' AS three, f.* FROM FLOAT4_TBL f WHERE  f.f1 < '1004.3';

SELECT '' AS four, f.* FROM FLOAT4_TBL f WHERE '1004.3' >= f.f1;

SELECT '' AS four, f.* FROM FLOAT4_TBL f WHERE  f.f1 <= '1004.3';

SELECT '' AS three, f.f1, f.f1 * '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS three, f.f1, f.f1 + '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS three, f.f1, f.f1 / '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT '' AS three, f.f1, f.f1 - '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

-- test divide by zero
SELECT '' AS bad, f.f1 / '0.0' from FLOAT4_TBL f;

SELECT '' AS five, FLOAT4_TBL.*;

-- test the unary float4abs operator 
SELECT '' AS five, f.f1, @f.f1 AS abs_f1 FROM FLOAT4_TBL f;

UPDATE FLOAT4_TBL
   SET f1 = FLOAT4_TBL.f1 * '-1'
   WHERE FLOAT4_TBL.f1 > '0.0';

SELECT '' AS five, FLOAT4_TBL.*;

