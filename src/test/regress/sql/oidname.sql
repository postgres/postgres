-- *************testing built-in type oidname ****************
-- oidname is a an adt for multiple key indices involving oid and name
-- probably will not be used directly by most users 

CREATE TABLE OIDNAME_TBL(f1 oidname);

INSERT INTO OIDNAME_TBL(f1) VALUES ('1234,abcd');

INSERT INTO OIDNAME_TBL(f1) VALUES ('1235,efgh');

INSERT INTO OIDNAME_TBL(f1) VALUES ('987,XXXX');

-- no char16 component 
INSERT INTO OIDNAME_TBL(f1) VALUES ('123456');

-- char16 component too long 
INSERT INTO OIDNAME_TBL(f1) VALUES ('123456,abcdefghijklmnopqrsutvwyz');

-- bad inputs 
INSERT INTO OIDNAME_TBL(f1) VALUES ('');

INSERT INTO OIDNAME_TBL(f1) VALUES ('asdfasd');


SELECT '' AS four, OIDNAME_TBL.*;

SELECT '' AS one, o.* FROM OIDNAME_TBL o WHERE o.f1 = '1234,abcd';

SELECT '' AS three, o.* FROM OIDNAME_TBL o WHERE o.f1 <> '1234,abcd';

SELECT '' AS two, o.* FROM OIDNAME_TBL o WHERE o.f1 <= '1234,abcd';

SELECT '' AS one, o.* FROM OIDNAME_TBL o WHERE o.f1 < '1234,abcd';

SELECT '' AS three, o.* FROM OIDNAME_TBL o WHERE o.f1 >= '1234,abcd';

SELECT '' AS two, o.* FROM OIDNAME_TBL o WHERE o.f1 > '1234,abcd';


