--**************** testing built-in type char4 **************
--
-- all inputs are silently truncated at 4 characters
--

CREATE TABLE CHAR4_TBL (f1  char4);

INSERT INTO CHAR4_TBL(f1) VALUES ('ABCD');

INSERT INTO CHAR4_TBL(f1) VALUES ('abcd');

INSERT INTO CHAR4_TBL(f1) VALUES ('ZYWZ');

INSERT INTO CHAR4_TBL(f1) VALUES ('343f');

INSERT INTO CHAR4_TBL(f1) VALUES ('d34a');

INSERT INTO CHAR4_TBL(f1) VALUES ('');

INSERT INTO CHAR4_TBL(f1) VALUES ('12345678');


SELECT '' AS seven, CHAR4_TBL.*;

SELECT '' AS six, c.f1 FROM CHAR4_TBL c WHERE c.f1 <> 'ABCD';

SELECT '' AS one, c.f1 FROM CHAR4_TBL c WHERE c.f1 = 'ABCD';

SELECT '' AS three, c.f1 FROM CHAR4_TBL c WHERE c.f1 < 'ABCD';

SELECT '' AS four, c.f1 FROM CHAR4_TBL c WHERE c.f1 <= 'ABCD';

SELECT '' AS three, c.f1 FROM CHAR4_TBL c WHERE c.f1 > 'ABCD';

SELECT '' AS four, c.f1 FROM CHAR4_TBL c WHERE c.f1 >= 'ABCD';

SELECT '' AS seven, c.f1 FROM CHAR4_TBL c WHERE c.f1 ~ '.*';

SELECT '' AS zero, c.f1 FROM CHAR4_TBL c WHERE c.f1 !~ '.*';

SELECT '' AS three, c.f1 FROM CHAR4_TBL c WHERE c.f1 ~ '.*34.*';

DROP TABLE  CHAR4_TBL;
