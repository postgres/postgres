--
-- STRINGS
-- Test various data entry syntaxes.
--

-- SQL92 string continuation syntax
-- E021-03 character string literals
SELECT 'first line'
' - next line'
	' - third line'
	AS "Three lines to one";

-- illegal string continuation syntax
SELECT 'first line'
' - next line' /* this comment is not allowed here */
' - third line'
	AS "Illegal comment within continuation";

--
-- test conversions between various string types
-- E021-10 implicit casting among the character data types
--

SELECT CAST(f1 AS text) AS "text(char)" FROM CHAR_TBL;

SELECT CAST(f1 AS text) AS "text(varchar)" FROM VARCHAR_TBL;

SELECT CAST(name 'namefield' AS text) AS "text(name)";

SELECT CAST(f1 AS char(10)) AS "char(text)" FROM TEXT_TBL; -- fail

SELECT CAST(f1 AS char(20)) AS "char(text)" FROM TEXT_TBL;

SELECT CAST(f1 AS char(10)) AS "char(varchar)" FROM VARCHAR_TBL;

SELECT CAST(name 'namefield' AS char(10)) AS "char(name)";

SELECT CAST(f1 AS varchar) AS "varchar(text)" FROM TEXT_TBL;

SELECT CAST(f1 AS varchar) AS "varchar(char)" FROM CHAR_TBL;

SELECT CAST(name 'namefield' AS varchar) AS "varchar(name)";

--
-- test SQL92 string functions
-- E### and T### are feature reference numbers from SQL99
--

-- E021-09 trim function
SELECT TRIM(BOTH FROM '  bunch o blanks  ') = 'bunch o blanks' AS "bunch o blanks";

SELECT TRIM(LEADING FROM '  bunch o blanks  ') = 'bunch o blanks  ' AS "bunch o blanks  ";

SELECT TRIM(TRAILING FROM '  bunch o blanks  ') = '  bunch o blanks' AS "  bunch o blanks";

SELECT TRIM(BOTH 'x' FROM 'xxxxxsome Xsxxxxx') = 'some Xs' AS "some Xs";

-- E021-06 substring expression
SELECT SUBSTRING('1234567890' FROM 3) = '34567890' AS "34567890";

SELECT SUBSTRING('1234567890' FROM 4 FOR 3) = '456' AS "456";

-- T581 regular expression substring
SELECT SUBSTRING('abcdefg' FROM '(b|f).*(d)' FOR '#') AS "bcd";

-- No match should return NULL
SELECT SUBSTRING('abcdefg' FROM '(1|2|3)' FOR '#') IS NULL AS "True";

-- Null inputs should return NULL
SELECT SUBSTRING('abcdefg' FROM '(b|c)' FOR NULL) IS NULL AS "True";
SELECT SUBSTRING(NULL FROM '(b|c)' FOR '#') IS NULL AS "True";
SELECT SUBSTRING('abcdefg' FROM NULL FOR '#') IS NULL AS "True";

-- PostgreSQL extention to allow omitting the escape character
SELECT SUBSTRING('abcdefg' FROM '(c|d).e') AS "cde";

-- E021-11 position expression
SELECT POSITION('4' IN '1234567890') = '4' AS "4";

SELECT POSITION(5 IN '1234567890') = '5' AS "5";

-- T312 character overlay function
SELECT OVERLAY('abcdef' PLACING '45' FROM 4) AS "abc45f";

SELECT OVERLAY('yabadoo' PLACING 'daba' FROM 5) AS "yabadaba";

SELECT OVERLAY('yabadoo' PLACING 'daba' FROM 5 FOR 0) AS "yabadabadoo";

SELECT OVERLAY('babosa' PLACING 'ubb' FROM 2 FOR 4) AS "bubba";

--
-- test LIKE
-- Be sure to form every test as a LIKE/NOT LIKE pair.
--

-- simplest examples
-- E061-04 like predicate
SELECT 'hawkeye' LIKE 'h%' AS "true";
SELECT 'hawkeye' NOT LIKE 'h%' AS "false";

SELECT 'hawkeye' LIKE 'H%' AS "false";
SELECT 'hawkeye' NOT LIKE 'H%' AS "true";

SELECT 'hawkeye' LIKE 'indio%' AS "false";
SELECT 'hawkeye' NOT LIKE 'indio%' AS "true";

SELECT 'hawkeye' LIKE 'h%eye' AS "true";
SELECT 'hawkeye' NOT LIKE 'h%eye' AS "false";

SELECT 'indio' LIKE '_ndio' AS "true";
SELECT 'indio' NOT LIKE '_ndio' AS "false";

SELECT 'indio' LIKE 'in__o' AS "true";
SELECT 'indio' NOT LIKE 'in__o' AS "false";

SELECT 'indio' LIKE 'in_o' AS "false";
SELECT 'indio' NOT LIKE 'in_o' AS "true";

-- unused escape character
SELECT 'hawkeye' LIKE 'h%' ESCAPE '#' AS "true";
SELECT 'hawkeye' NOT LIKE 'h%' ESCAPE '#' AS "false";

SELECT 'indio' LIKE 'ind_o' ESCAPE '$' AS "true";
SELECT 'indio' NOT LIKE 'ind_o' ESCAPE '$' AS "false";

-- escape character
-- E061-05 like predicate with escape clause
SELECT 'h%' LIKE 'h#%' ESCAPE '#' AS "true";
SELECT 'h%' NOT LIKE 'h#%' ESCAPE '#' AS "false";

SELECT 'h%wkeye' LIKE 'h#%' ESCAPE '#' AS "false";
SELECT 'h%wkeye' NOT LIKE 'h#%' ESCAPE '#' AS "true";

SELECT 'h%wkeye' LIKE 'h#%%' ESCAPE '#' AS "true";
SELECT 'h%wkeye' NOT LIKE 'h#%%' ESCAPE '#' AS "false";

SELECT 'h%awkeye' LIKE 'h#%a%k%e' ESCAPE '#' AS "true";
SELECT 'h%awkeye' NOT LIKE 'h#%a%k%e' ESCAPE '#' AS "false";

SELECT 'indio' LIKE '_ndio' ESCAPE '$' AS "true";
SELECT 'indio' NOT LIKE '_ndio' ESCAPE '$' AS "false";

SELECT 'i_dio' LIKE 'i$_d_o' ESCAPE '$' AS "true";
SELECT 'i_dio' NOT LIKE 'i$_d_o' ESCAPE '$' AS "false";

SELECT 'i_dio' LIKE 'i$_nd_o' ESCAPE '$' AS "false";
SELECT 'i_dio' NOT LIKE 'i$_nd_o' ESCAPE '$' AS "true";

SELECT 'i_dio' LIKE 'i$_d%o' ESCAPE '$' AS "true";
SELECT 'i_dio' NOT LIKE 'i$_d%o' ESCAPE '$' AS "false";

-- escape character same as pattern character
SELECT 'maca' LIKE 'm%aca' ESCAPE '%' AS "true";
SELECT 'maca' NOT LIKE 'm%aca' ESCAPE '%' AS "false";

SELECT 'ma%a' LIKE 'm%a%%a' ESCAPE '%' AS "true";
SELECT 'ma%a' NOT LIKE 'm%a%%a' ESCAPE '%' AS "false";

SELECT 'bear' LIKE 'b_ear' ESCAPE '_' AS "true";
SELECT 'bear' NOT LIKE 'b_ear' ESCAPE '_' AS "false";

SELECT 'be_r' LIKE 'b_e__r' ESCAPE '_' AS "true";
SELECT 'be_r' NOT LIKE 'b_e__r' ESCAPE '_' AS "false";

SELECT 'be_r' LIKE '__e__r' ESCAPE '_' AS "false";
SELECT 'be_r' NOT LIKE '__e__r' ESCAPE '_' AS "true";


--
-- test ILIKE (case-insensitive LIKE)
-- Be sure to form every test as an ILIKE/NOT ILIKE pair.
--

SELECT 'hawkeye' ILIKE 'h%' AS "true";
SELECT 'hawkeye' NOT ILIKE 'h%' AS "false";

SELECT 'hawkeye' ILIKE 'H%' AS "true";
SELECT 'hawkeye' NOT ILIKE 'H%' AS "false";

SELECT 'hawkeye' ILIKE 'H%Eye' AS "true";
SELECT 'hawkeye' NOT ILIKE 'H%Eye' AS "false";

SELECT 'Hawkeye' ILIKE 'h%' AS "true";
SELECT 'Hawkeye' NOT ILIKE 'h%' AS "false";

--
-- test implicit type conversion
--

-- E021-07 character concatenation
SELECT 'unknown' || ' and unknown' AS "Concat unknown types";

SELECT text 'text' || ' and unknown' AS "Concat text to unknown type";

SELECT char(20) 'characters' || 'and text' AS "Concat char to unknown type";

SELECT text 'text' || char(20) ' and characters' AS "Concat text to char";

SELECT text 'text' || varchar ' and varchar' AS "Concat text to varchar";
