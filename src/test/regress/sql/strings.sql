--
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
' - third line';

--
-- test conversions between various string types
--

SELECT text(f1) FROM CHAR_TBL;

SELECT text(f1) FROM VARCHAR_TBL;

