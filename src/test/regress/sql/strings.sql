--
-- STRINGS
-- Test various data entry syntaxes.
--

-- SQL92 string continuation syntax
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
--

SELECT CAST(f1 AS text) AS "text(char)" FROM CHAR_TBL;

SELECT CAST(f1 AS text) AS "text(varchar)" FROM VARCHAR_TBL;

SELECT CAST(name 'namefield' AS text) AS "text(name)";

SELECT CAST(f1 AS char(10)) AS "char(text)" FROM TEXT_TBL;

SELECT CAST(f1 AS char(10)) AS "char(varchar)" FROM VARCHAR_TBL;

SELECT CAST(name 'namefield' AS char(10)) AS "char(name)";

SELECT CAST(f1 AS varchar) AS "varchar(text)" FROM TEXT_TBL;

SELECT CAST(f1 AS varchar) AS "varchar(char)" FROM CHAR_TBL;

SELECT CAST(name 'namefield' AS varchar) AS "varchar(name)";

--
-- test SQL92 string functions
--

SELECT TRIM(BOTH FROM '  bunch o blanks  ') AS "bunch o blanks";

SELECT TRIM(LEADING FROM '  bunch o blanks  ') AS "bunch o blanks  ";

SELECT TRIM(TRAILING FROM '  bunch o blanks  ') AS "  bunch o blanks";

SELECT TRIM(BOTH 'x' FROM 'xxxxxsome Xsxxxxx') AS "some Xs";

SELECT SUBSTRING('1234567890' FROM 3) AS "34567890";

SELECT SUBSTRING('1234567890' FROM 4 FOR 3) AS "456";

SELECT POSITION('4' IN '1234567890') AS "4";

SELECT POSITION(5 IN '1234567890') AS "5";

--
-- test implicit type conversion
--

SELECT 'unknown' || ' and unknown' AS "Concat unknown types";

SELECT text 'text' || ' and unknown' AS "Concat text to unknown type";

SELECT text 'text' || char(10) ' and characters' AS "Concat text to char";

SELECT text 'text' || varchar ' and varchar' AS "Concat text to varchar";

