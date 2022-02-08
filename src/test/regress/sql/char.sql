--
-- CHAR
--

-- fixed-length by value
-- internally passed by value if <= 4 bytes in storage

SELECT char 'c' = char 'c' AS true;

--
-- Build a table for testing
-- (This temporarily hides the table created in test_setup.sql)
--

CREATE TEMP TABLE CHAR_TBL(f1 char);

INSERT INTO CHAR_TBL (f1) VALUES ('a');

INSERT INTO CHAR_TBL (f1) VALUES ('A');

-- any of the following three input formats are acceptable
INSERT INTO CHAR_TBL (f1) VALUES ('1');

INSERT INTO CHAR_TBL (f1) VALUES (2);

INSERT INTO CHAR_TBL (f1) VALUES ('3');

-- zero-length char
INSERT INTO CHAR_TBL (f1) VALUES ('');

-- try char's of greater than 1 length
INSERT INTO CHAR_TBL (f1) VALUES ('cd');
INSERT INTO CHAR_TBL (f1) VALUES ('c     ');


SELECT * FROM CHAR_TBL;

SELECT c.*
   FROM CHAR_TBL c
   WHERE c.f1 <> 'a';

SELECT c.*
   FROM CHAR_TBL c
   WHERE c.f1 = 'a';

SELECT c.*
   FROM CHAR_TBL c
   WHERE c.f1 < 'a';

SELECT c.*
   FROM CHAR_TBL c
   WHERE c.f1 <= 'a';

SELECT c.*
   FROM CHAR_TBL c
   WHERE c.f1 > 'a';

SELECT c.*
   FROM CHAR_TBL c
   WHERE c.f1 >= 'a';

DROP TABLE CHAR_TBL;

--
-- Now test longer arrays of char
--
-- This char_tbl was already created and filled in test_setup.sql.
-- Here we just try to insert bad values.
--

INSERT INTO CHAR_TBL (f1) VALUES ('abcde');

SELECT * FROM CHAR_TBL;
