--*************testing built-in type oidint4 ****************
-- oidint4 is a an adt for multiple key indices involving oid and int4 
-- probably will not be used directly by most users 

CREATE TABLE OIDINT4_TBL(f1 oidint4);

INSERT INTO OIDINT4_TBL(f1) VALUES ('1234/9873');

INSERT INTO OIDINT4_TBL(f1) VALUES ('1235/9873');

INSERT INTO OIDINT4_TBL(f1) VALUES ('987/-1234');

-- no int4 component 
--
-- this is defined as good in the code -- I don't know what will break
-- if we disallow it.
--
INSERT INTO OIDINT4_TBL(f1) VALUES ('123456');

-- int4 component too large 
INSERT INTO OIDINT4_TBL(f1) VALUES ('123456/1234568901234567890');

--
-- this is defined as good in the code -- I don't know what will break
-- if we disallow it.
--
INSERT INTO OIDINT4_TBL(f1) VALUES ('');

-- bad inputs 
INSERT INTO OIDINT4_TBL(f1) VALUES ('asdfasd');

SELECT '' AS five, OIDINT4_TBL.*;

SELECT '' AS one, o.* FROM OIDINT4_TBL o WHERE o.f1 = '1235/9873';

SELECT '' AS four, o.* FROM OIDINT4_TBL o WHERE o.f1 <> '1235/9873';

SELECT '' AS four, o.* FROM OIDINT4_TBL o WHERE o.f1 <= '1235/9873';

SELECT '' AS three, o.* FROM OIDINT4_TBL o WHERE o.f1 < '1235/9873';

SELECT '' AS two, o.* FROM OIDINT4_TBL o WHERE o.f1 >= '1235/9873';

SELECT '' AS one, o.* FROM OIDINT4_TBL o WHERE o.f1 > '1235/9873';

DROP TABLE  OIDINT4_TBL;
