--**************** testing built-in type char16 **************
--
-- all inputs are silently truncated at 16 characters
--

-- fixed-length by reference
SELECT 'char 16 string'::char16 = 'char 16 string'::char16 AS "True";

SELECT 'char 16 string'::char16 = 'char 16 string '::char16 AS "False";

--
--
--

CREATE TABLE CHAR16_TBL(f1 char16);

INSERT INTO CHAR16_TBL(f1) VALUES ('ABCDEFGHIJKLMNOP');

INSERT INTO CHAR16_TBL(f1) VALUES ('abcdefghijklmnop');

INSERT INTO CHAR16_TBL(f1) VALUES ('asdfghjkl;');

INSERT INTO CHAR16_TBL(f1) VALUES ('343f%2a');

INSERT INTO CHAR16_TBL(f1) VALUES ('d34aaasdf');

INSERT INTO CHAR16_TBL(f1) VALUES ('');

INSERT INTO CHAR16_TBL(f1) VALUES ('1234567890ABCDEFGHIJKLMNOPQRSTUV');


SELECT '' AS seven, CHAR16_TBL.*;

SELECT '' AS six, c.f1 FROM CHAR16_TBL c WHERE c.f1 <> 'ABCDEFGHIJKLMNOP';

SELECT '' AS one, c.f1 FROM CHAR16_TBL c WHERE c.f1 = 'ABCDEFGHIJKLMNOP';

SELECT '' AS three, c.f1 FROM CHAR16_TBL c WHERE c.f1 < 'ABCDEFGHIJKLMNOP';

SELECT '' AS four, c.f1 FROM CHAR16_TBL c WHERE c.f1 <= 'ABCDEFGHIJKLMNOP';

SELECT '' AS three, c.f1 FROM CHAR16_TBL c WHERE c.f1 > 'ABCDEFGHIJKLMNOP';

SELECT '' AS four, c.f1 FROM CHAR16_TBL c WHERE c.f1 >= 'ABCDEFGHIJKLMNOP';

SELECT '' AS seven, c.f1 FROM CHAR16_TBL c WHERE c.f1 ~ '.*';

SELECT '' AS zero, c.f1 FROM CHAR16_TBL c WHERE c.f1 !~ '.*';

SELECT '' AS three, c.f1 FROM CHAR16_TBL c WHERE c.f1 ~ '[0-9]';

SELECT '' AS two, c.f1 FROM CHAR16_TBL c WHERE c.f1 ~ '.*asdf.*';

DROP TABLE CHAR16_TBL;
