--  **************** testing built-in type char8 **************
--
-- all inputs are silently truncated at 8 characters
--

CREATE TABLE CHAR8_TBL(f1 char8);

INSERT INTO CHAR8_TBL(f1) VALUES ('ABCDEFGH');

INSERT INTO CHAR8_TBL(f1) VALUES ('abcdefgh');

INSERT INTO CHAR8_TBL(f1) VALUES ('ZYWZ410-');

INSERT INTO CHAR8_TBL(f1) VALUES ('343f%2a');

INSERT INTO CHAR8_TBL(f1) VALUES ('d34aas');

INSERT INTO CHAR8_TBL(f1) VALUES ('');

INSERT INTO CHAR8_TBL(f1) VALUES ('1234567890');


SELECT '' AS seven, CHAR8_TBL.*;

SELECT '' AS six, c.f1 FROM CHAR8_TBL c WHERE c.f1 <> 'ABCDEFGH';

SELECT '' AS one, c.f1 FROM CHAR8_TBL c WHERE c.f1 = 'ABCDEFGH';

SELECT '' AS three, c.f1 FROM CHAR8_TBL c WHERE c.f1 < 'ABCDEFGH';

SELECT '' AS four, c.f1 FROM CHAR8_TBL c WHERE c.f1 <= 'ABCDEFGH';

SELECT '' AS three, c.f1 FROM CHAR8_TBL c WHERE c.f1 > 'ABCDEFGH';

SELECT '' AS four, c.f1 FROM CHAR8_TBL c WHERE c.f1 >= 'ABCDEFGH';

SELECT '' AS seven, c.f1 FROM CHAR8_TBL c WHERE c.f1 ~ '.*';

SELECT '' AS zero, c.f1 FROM CHAR8_TBL c WHERE c.f1 !~ '.*';

SELECT '' AS four, c.f1 FROM CHAR8_TBL c WHERE c.f1 ~ '[0-9]';

SELECT '' AS three, c.f1 FROM CHAR8_TBL c WHERE c.f1 ~ '.*34.*';

DROP TABLE  CHAR8_TBL;
