-- PostgreSQL catalog extensions for ODBC compatibility
-- $Header: /cvsroot/pgsql/src/interfaces/odbc/Attic/odbc.sql,v 1.2 2001/10/09 22:32:33 petere Exp $

-- ODBC functions are described here:
-- <http://msdn.microsoft.com/library/en-us/odbc/htm/odbcscalar_functions.asp>

-- Note:  If we format this file consistently we can automatically
-- generate a corresponding "drop script".  Start "CREATE" in the first
-- column, and keep everything up to and including the argument list on
-- the same line.  See also the makefile rule.


-- String Functions
-- ++++++++++++++++
--
-- Built-in: ASCII, BIT_LENGTH, CHAR_LENGTH, CHARACTER_LENGTH, LTRIM,
--           OCTET_LENGTH, POSITION, REPEAT, RTRIM, SUBSTRING
-- Missing: DIFFERENCE, REPLACE, SOUNDEX, LENGTH (ODBC sense)
-- Keyword problems: CHAR, LEFT, RIGHT


-- CHAR(code)
CREATE OR REPLACE FUNCTION "char"(integer) RETURNS text AS '
    SELECT chr($1);
' LANGUAGE SQL;


-- CONCAT(string1, string2)
CREATE OR REPLACE FUNCTION concat(text, text) RETURNS text AS '
    SELECT $1 || $2;
' LANGUAGE SQL;


-- INSERT(string1, start, len, string2)
CREATE OR REPLACE FUNCTION insert(text, integer, integer, text) RETURNS text AS '
    SELECT substring($1 from 1 for $2) || $4 || substring($1 from $2 + $3 + 1);
' LANGUAGE SQL;


-- LCASE(string)
CREATE OR REPLACE FUNCTION lcase(text) RETURNS text AS '
    SELECT lower($1);
' LANGUAGE SQL;


-- LEFT(string, count)
CREATE OR REPLACE FUNCTION "left"(text, integer) RETURNS text AS '
    SELECT substring($1 for $2);
' LANGUAGE SQL;


-- LOCATE(substring, string[, start])
CREATE OR REPLACE FUNCTION locate(text, text) RETURNS integer AS '
    SELECT position($1 in $2);
' LANGUAGE SQL;
CREATE OR REPLACE FUNCTION locate(text, text, integer) RETURNS integer AS '
    SELECT position($1 in substring($2 from $3)) + $3 - 1;
' LANGUAGE SQL;


-- RIGHT(string, count)
CREATE OR REPLACE FUNCTION "right"(text, integer) RETURNS text AS '
    SELECT substring($1 from char_length($1) - $2 + 1);
' LANGUAGE SQL;


-- SPACE(count)
CREATE OR REPLACE FUNCTION space(integer) RETURNS text AS '
    SELECT repeat('' '', $1);
' LANGUAGE SQL;


-- UCASE(string)
CREATE OR REPLACE FUNCTION ucase(text) RETURNS text AS '
    SELECT upper($1);
' LANGUAGE SQL;


-- Numeric Functions
-- +++++++++++++++++
--
-- Built-in: ABS, ACOS, ASIN, ATAN, ATAN2, COS, COT, DEGRESS, EXP,
--           FLOOR, MOD, PI, RADIANS, ROUND, SIGN, SIN, SQRT, TAN
-- Missing: LOG (ODBC sense)


-- CEILING(num)
CREATE OR REPLACE FUNCTION ceiling(numeric) RETURNS numeric AS '
    SELECT ceil($1);
' LANGUAGE SQL;


-- LOG10(num)
CREATE OR REPLACE FUNCTION log10(double precision) RETURNS double precision AS '
    SELECT log($1);
' LANGUAGE SQL;
CREATE OR REPLACE FUNCTION log10(numeric) RETURNS numeric AS '
    SELECT log($1);
' LANGUAGE SQL;


-- POWER(num, num)
CREATE OR REPLACE FUNCTION power(double precision, double precision)
  RETURNS double precision AS '
    SELECT pow($1, $2);
' LANGUAGE SQL;
CREATE OR REPLACE FUNCTION power(numeric, numeric)
  RETURNS numeric AS '
    SELECT pow($1, $2);
' LANGUAGE SQL;


-- RAND([seed])
CREATE OR REPLACE FUNCTION rand() RETURNS double precision AS '
    SELECT random();
' LANGUAGE SQL;
CREATE OR REPLACE FUNCTION rand(double precision) RETURNS double precision AS '
    SELECT setseed($1);
    SELECT random();
' LANGUAGE SQL;


-- TRUNCATE(num, places)
CREATE OR REPLACE FUNCTION truncate(numeric, integer) RETURNS numeric AS '
    SELECT trunc($1, $2);
' LANGUAGE SQL;


-- Time, Date, and Interval Functions
-- ++++++++++++++++++++++++++++++++++
--
-- Built-in: CURRENT_DATE, CURRENT_TIME, CURRENT_TIMESTAMP, EXTRACT, NOW
-- Missing: none


CREATE OR REPLACE FUNCTION curdate() RETURNS date AS '
    SELECT current_date;
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION curtime() RETURNS time with time zone AS '
    SELECT current_time;
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION dayname(timestamp) RETURNS text AS '
    SELECT to_char($1,''Day'');
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION dayofmonth(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(day FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION dayofweek(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(dow FROM $1) AS integer) + 1;
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION dayofyear(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(doy FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION hour(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(hour FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION minute(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(minute FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION month(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(month FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION monthname(timestamp) RETURNS text AS '
    SELECT to_char($1, ''Month'');
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION quarter(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(quarter FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION second(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(second FROM $1) AS integer);
' LANGUAGE SQL;

/*
-- The first argument is an integer constant denoting the units
-- of the second argument. Until we know the actual values, we
-- cannot implement these. - thomas 2000-04-11
xCREATE OR REPLACE FUNCTION timestampadd(integer, integer, timestamp)
  RETURNS timestamp AS '
    SELECT CAST(($3 + ($2 * $1)) AS timestamp);
' LANGUAGE SQL;

xCREATE OR REPLACE FUNCTION timestampdiff(integer, integer, timestamp)
  RETURNS timestamp AS '
    SELECT CAST(($3 + ($2 * $1)) AS timestamp);
' LANGUAGE SQL;
*/

CREATE OR REPLACE FUNCTION week(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(week FROM $1) AS integer);
' LANGUAGE SQL;

CREATE OR REPLACE FUNCTION year(timestamp) RETURNS integer AS '
    SELECT CAST(EXTRACT(year FROM $1) AS integer);
' LANGUAGE SQL;


-- System Functions
-- ++++++++++++++++
--
-- Built-in: USER
-- Missing: DATABASE, IFNULL
