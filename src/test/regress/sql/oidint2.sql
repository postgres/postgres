-- *************testing built-in type oidint2 ****************
-- oidint2 is a an adt for multiple key indices involving oid and int2 
-- probably will not be used directly by most users 

CREATE TABLE OIDINT2_TBL(f1 oidint2);

INSERT INTO OIDINT2_TBL(f1) VALUES ('1234/9873');

INSERT INTO OIDINT2_TBL(f1) VALUES ('1235/9873');

INSERT INTO OIDINT2_TBL(f1) VALUES ('987/-1234');

-- no int2 component 
--
-- this is defined as good in the code -- I don't know what will break
-- if we disallow it.
--
INSERT INTO OIDINT2_TBL(f1) VALUES ('123456');

-- int2 component too large 
INSERT INTO OIDINT2_TBL(f1) VALUES ('123456/123456');

--
-- this is defined as good in the code -- I don't know what will break
-- if we disallow it.
--
INSERT INTO OIDINT2_TBL(f1) VALUES ('');

-- bad inputs 
INSERT INTO OIDINT2_TBL(f1) VALUES ('asdfasd');


SELECT '' AS five, OIDINT2_TBL.*;

SELECT '' AS one, o.* FROM OIDINT2_TBL o WHERE o.f1 = '1235/9873';

SELECT '' AS four, o.* FROM OIDINT2_TBL o WHERE o.f1 <> '1235/9873';

SELECT '' AS four, o.* FROM OIDINT2_TBL o WHERE o.f1 <= '1235/9873';

SELECT '' AS three, o.* FROM OIDINT2_TBL o WHERE o.f1 < '1235/9873';

SELECT '' AS two, o.* FROM OIDINT2_TBL o WHERE o.f1 >= '1235/9873';

SELECT '' AS one, o.* FROM OIDINT2_TBL o WHERE o.f1 > '1235/9873';

DROP TABLE  OIDINT2_TBL;
