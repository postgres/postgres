---------------------------------------------------------------------------
--
-- basics.sql-
--    Tutorial on the basics (table creation and data manipulation)
--
--
-- src/tutorial/basics.source
--
---------------------------------------------------------------------------

-----------------------------
-- Creating a New Table:
--	A CREATE TABLE is used to create base tables.  PostgreSQL has
--	its own set of built-in types.  (Note that SQL is case-
--	insensitive.)
-----------------------------

CREATE TABLE weather (
	city		varchar(80),
	temp_lo		int,		-- low temperature
	temp_hi		int,		-- high temperature
	prcp		real,		-- precipitation
	date		date
);

CREATE TABLE cities (
	name		varchar(80),
	location	point
);


-----------------------------
-- Populating a Table With Rows:
--	An INSERT statement is used to insert a new row into a table.  There
--	are several ways you can specify what columns the data should go to.
-----------------------------

-- 1. The simplest case is when the list of value correspond to the order of
--    the columns specified in CREATE TABLE.

INSERT INTO weather
    VALUES ('San Francisco', 46, 50, 0.25, '1994-11-27');

INSERT INTO cities
    VALUES ('San Francisco', '(-194.0, 53.0)');

-- 2. You can also specify what column the values correspond to.  (The columns
--    can be specified in any order.  You may also omit any number of columns,
--    e.g., unknown precipitation below.

INSERT INTO weather (city, temp_lo, temp_hi, prcp, date)
    VALUES ('San Francisco', 43, 57, 0.0, '1994-11-29');

INSERT INTO weather (date, city, temp_hi, temp_lo)
    VALUES ('1994-11-29', 'Hayward', 54, 37);


-----------------------------
-- Querying a Table:
--	A SELECT statement is used for retrieving data.  The basic syntax is
--	SELECT columns FROM tables WHERE predicates.
-----------------------------

-- A simple one would be:

SELECT * FROM weather;

-- You may also specify expressions in the target list.  (The 'AS column'
-- specifies the column name of the result.  It is optional.)

SELECT city, (temp_hi+temp_lo)/2 AS temp_avg, date FROM weather;

-- If you want to retrieve rows that satisfy certain condition (i.e., a
-- restriction), specify the condition in WHERE.  The following retrieves
-- the weather of San Francisco on rainy days.

SELECT *
    FROM weather
    WHERE city = 'San Francisco'
        AND prcp > 0.0;

-- Here is a more complicated one.  Duplicates are removed when DISTINCT is
-- specified. ORDER BY specifies the column to sort on.  (Just to make sure the
-- following won't confuse you, DISTINCT and ORDER BY can be used separately.)

SELECT DISTINCT city
    FROM weather
    ORDER BY city;


-----------------------------
-- Joins Between Tables:
--	queries can access multiple tables at once or access the same table
--	in such a way that multiple instances of the table are being processed
--	at the same time.
-----------------------------

-- The following joins the weather table and the cities table.

SELECT *
    FROM weather, cities
    WHERE city = name;

-- This prevents a duplicate city name column:

SELECT city, temp_lo, temp_hi, prcp, date, location
    FROM weather, cities
    WHERE city = name;

-- since the column names are all different, we don't have to specify the
-- table name. If you want to be clear, you can do the following. They give
-- identical results, of course.

SELECT weather.city, weather.temp_lo, weather.temp_hi, weather.prcp, weather.date, cities.location
    FROM weather, cities
    WHERE cities.name = weather.city;

-- JOIN syntax

SELECT *
    FROM weather JOIN cities ON (weather.city = cities.name);

-- Outer join

SELECT *
    FROM weather LEFT OUTER JOIN cities ON (weather.city = cities.name);

-- Suppose we want to find all the records that are in the temperature range
-- of other records.  W1 and W2 are aliases for weather.

SELECT W1.city, W1.temp_lo, W1.temp_hi,
       W2.city, W2.temp_lo, W2.temp_hi
FROM weather W1, weather W2
WHERE W1.temp_lo < W2.temp_lo
   and W1.temp_hi > W2.temp_hi;


-----------------------------
-- Aggregate Functions
-----------------------------

SELECT max(temp_lo)
    FROM weather;

SELECT city FROM weather
    WHERE temp_lo = (SELECT max(temp_lo) FROM weather);

-- Aggregate with GROUP BY
SELECT city, max(temp_lo)
    FROM weather
    GROUP BY city;

-- ... and HAVING
SELECT city, max(temp_lo)
    FROM weather
    GROUP BY city
    HAVING max(temp_lo) < 40;


-----------------------------
-- Updates:
--	An UPDATE statement is used for updating data.
-----------------------------

-- Suppose you discover the temperature readings are all off by 2 degrees as
-- of Nov 28, you may update the data as follow:

UPDATE weather
    SET temp_hi = temp_hi - 2,  temp_lo = temp_lo - 2
    WHERE date > '1994-11-28';

SELECT * FROM weather;


-----------------------------
-- Deletions:
--	A DELETE statement is used for deleting rows from a table.
-----------------------------

-- Suppose you are no longer interested in the weather of Hayward, then you can
-- do the following to delete those rows from the table.

DELETE FROM weather WHERE city = 'Hayward';

SELECT * FROM weather;

-- You can also delete all the rows in a table by doing the following.  (This
-- is different from DROP TABLE which removes the table in addition to the
-- removing the rows.)

DELETE FROM weather;

SELECT * FROM weather;


-----------------------------
-- Removing the tables:
--	DROP TABLE is used to remove tables.  After you have done this, you
--      can no longer use those tables.
-----------------------------

DROP TABLE weather, cities;
