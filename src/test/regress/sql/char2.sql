--  **************** testing built-in type char2 **************
--
-- all inputs are silently truncated at 2 characters
--

CREATE TABLE CHAR2_TBL(f1 char2);

INSERT INTO CHAR2_TBL (f1) VALUES ('AB');

INSERT INTO CHAR2_TBL (f1) VALUES ('ab');

INSERT INTO CHAR2_TBL (f1) VALUES ('ZY');

INSERT INTO CHAR2_TBL (f1) VALUES ('34');

INSERT INTO CHAR2_TBL (f1) VALUES ('d');

INSERT INTO CHAR2_TBL (f1) VALUES ('');

INSERT INTO CHAR2_TBL (f1) VALUES ('12345');


SELECT '' AS seven, CHAR2_TBL.*;

SELECT '' AS six, c.f1 FROM CHAR2_TBL c WHERE c.f1 <> 'AB';

SELECT '' AS one, c.f1 FROM CHAR2_TBL c WHERE c.f1 = 'AB';

SELECT '' AS three, c.f1 FROM CHAR2_TBL c WHERE c.f1 < 'AB';

SELECT '' AS four, c.f1 FROM CHAR2_TBL c WHERE c.f1 <= 'AB';

SELECT '' AS three, c.f1 FROM CHAR2_TBL c WHERE c.f1 > 'AB';

SELECT '' AS four, c.f1 FROM CHAR2_TBL c WHERE c.f1 >= 'AB';

SELECT '' AS seven, c.f1 FROM CHAR2_TBL c WHERE c.f1 ~ '.*';

SELECT '' AS zero, c.f1 FROM CHAR2_TBL c WHERE c.f1 !~ '.*';

SELECT '' AS one, c.f1 FROM CHAR2_TBL c WHERE c.f1 ~ '34';

SELECT '' AS one, c.f1 FROM CHAR2_TBL c WHERE c.f1 ~ '3.*';

DROP TABLE CHAR2_TBL;
