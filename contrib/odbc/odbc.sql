-- ODBC.sql
--

--
-- Character string manipulation
--

--
-- Extensions for ODBC compliance in v7.0.
-- In the current driver, ODBC functions must map directly into a
-- Postgres function. So in some cases we must create a compatible
-- function.
--

-- truncate on the left
CREATE FUNCTION ltrunc(text, integer)
    RETURNS text
    AS 'SELECT substring($1 FROM 1 FOR $2)'
    LANGUAGE 'SQL';

-- truncate on the right
CREATE FUNCTION rtrunc(text, integer)
    RETURNS text
    AS 'SELECT substring($1 FROM (char_length($1)-($2)+1) FOR $2)'
    LANGUAGE 'SQL';

CREATE FUNCTION space(integer)
    RETURNS text
    AS 'SELECT lpad('''', $1, '' '')'
    LANGUAGE 'SQL';

--
-- Mathematical functions
--

CREATE FUNCTION truncate(numeric,integer)
    RETURNS numeric
    AS 'SELECT trunc($1, $2)'
    LANGUAGE 'SQL';

--
-- Date/time functions for v7.0
--

CREATE FUNCTION curdate()
    RETURNS date
    AS 'SELECT CAST(''now'' AS date)'
    LANGUAGE 'SQL';

CREATE FUNCTION curtime()
    RETURNS time
    AS 'SELECT CAST(''now'' AS time)'
    LANGUAGE 'SQL';

CREATE FUNCTION dayname(timestamp)
    RETURNS text
    AS 'SELECT to_char($1,''Day'')'
    LANGUAGE 'SQL';

CREATE FUNCTION dayofmonth(timestamp)
    RETURNS integer
    AS 'SELECT  CAST(date_part(''day'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION dayofweek(timestamp)
    RETURNS integer
    AS 'SELECT ( CAST(date_part(''dow'', $1) AS integer) + 1)'
    LANGUAGE 'SQL';

CREATE FUNCTION dayofyear(timestamp)
    RETURNS integer
    AS 'SELECT  CAST(date_part(''doy'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION hour(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''hour'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION minute(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''minute'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION odbc_month(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''month'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION monthname(timestamp)
    RETURNS text
    AS 'SELECT to_char($1, ''Month'')'
    LANGUAGE 'SQL';

CREATE FUNCTION quarter(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''quarter'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION second(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''second'', $1) AS integer)'
    LANGUAGE 'SQL';

/*
-- The first argument is an integer constant denoting the units
-- of the second argument. Until we know the actual values, we
-- cannot implement these. - thomas 2000-04-11
CREATE FUNCTION timestampadd(integer,integer,timestamp)
    RETURNS timestamp
    AS 'SELECT CAST(($3 + ($2 * $1)) AS timestamp)'
    LANGUAGE 'SQL';

CREATE FUNCTION timestampdiff(integer,integer,timestamp)
    RETURNS timestamp
    AS 'SELECT CAST(($3 + ($2 * $1)) AS timestamp)'
    LANGUAGE 'SQL';
*/

CREATE FUNCTION week(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''week'', $1) AS integer)'
    LANGUAGE 'SQL';

CREATE FUNCTION year(timestamp)
    RETURNS integer
    AS 'SELECT CAST(date_part(''year'', $1) AS integer)'
    LANGUAGE 'SQL';

--
-- System functions.
--

/*
CREATE FUNCTION database()
    RETURNS text
    AS 'SELECT ...'
    LANGUAGE 'SQL';
*/

CREATE FUNCTION odbc_user()
    RETURNS text
    AS 'SELECT CAST(USER AS text)'
    LANGUAGE 'SQL';

